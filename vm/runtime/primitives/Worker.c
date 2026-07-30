// `Worker parallel:` -- run an Array of 0-argument blocks on OS THREADS that
// SHARE this heap, and answer an Array of their results.
//
// The share-nothing sibling is Isolate, which gets its own heap and therefore
// needs no coordination at all. This one is the opposite: every worker allocates
// out of the same nursery, so all of it rests on machinery that already exists
// and none of it is invented here --
//
//   * a mutator SET (memory/Heap.h). A worker joins it under gcLock, so it can
//     never appear while a collection is walking the list, and the collector
//     scans EVERY member's handle scopes, native frames and unwind records
//     (memory/Collector.c). That is what makes a handle held by the caller a
//     live root while a worker is the one collecting;
//   * a per-mutator TLAB, so the workers do not contend on every allocation;
//   * a per-mutator remembered set, spliced back into the heap's by
//     heapEndMutator, so a barrier a worker logged is not lost with its storage;
//   * a stop-the-world safepoint, which is why a worker may collect at all.
//
// ---------------------------------------------------------------------------
// THE CALLER IS BLOCKED FOR THE WHOLE JOIN, AND ITS FRAME STAYS ANCHORED
// ---------------------------------------------------------------------------
//
// Two separate obligations that are easy to confuse, because both are about the
// caller and only one of them is about waiting.
//
// BLOCKED, because a caller parked in osThreadJoin will not reach a safepoint
// poll. A worker that needs the world stopped would wait for it forever, and the
// caller is waiting for the worker: `Worker parallel:` would deadlock on the
// first collection any worker triggered. heapGcEnterBlocked publishes "this
// mutator is in a native wait, treat it as already safe", which is exactly true.
//
// ANCHORED, and for the whole join rather than just the allocation below,
// because being safe to ignore is not the same as having no roots. The collector
// still walks this mutator, and what it walks is the chain of compiled frames --
// so the frame underneath this primitive has to be reachable the entire time a
// worker might collect, not merely while this function is the one allocating.
// Releasing the anchor after `newArray` and keeping the threads running would
// leave the caller's live registers invisible to a collection a worker triggers.
//
// ---------------------------------------------------------------------------
// WHY THE TWO ARRAYS TRAVEL AS HANDLES AND THE BLOCK IS RE-READ PER WORKER
// ---------------------------------------------------------------------------
//
// A raw Value handed to a worker goes stale the first time anything moves it,
// and something will: the workers allocate. A handle's slot is what the collector
// UPDATES, so each worker reads `blocks->raw` when it is about to use it and gets
// the current address. The handles belong to the CALLER's scope, which outlives
// the join, and the caller is a mutator, so they are scanned.
//
// This is the fourth time this repository has paid for a baked pointer (an IC
// cell's selector, a CodeUnit's literals, nil/true/false, a block unit's frame).
// It is not going to be the fifth inside a primitive that hands objects to other
// threads.

#include "runtime/primitives/Shared.h"
#include "runtime/Collection.h"
#include "os/OsThread.h"
#include <stdlib.h>
#include <string.h>


typedef struct {
	// Both are the CALLER's handles, so the collector keeps them current.
	Object *blocks;
	Object *results;
	size_t index;
	Heap *heap;
	OsThread thread;
	// Did this element answer at all? A block that is not understood leaves the
	// slot alone rather than storing something plausible.
	_Bool answered;
} ParallelWork;


// One worker: become a mutator of the caller's heap, evaluate one block, store
// one result, leave the mutator set.
//
// NOT initThread (core/Thread.c), which CREATES a heap. Sharing the caller's is
// the entire difference between a Worker and an Isolate.
static void parallelWorkerBody(void *argument)
{
	ParallelWork *work = argument;

	// A zeroed Thread is the correct starting state and not merely a tidy one:
	// an EMPTY TLAB (top == end == NULL) sends the first allocation down the
	// refill path so it carves a chunk of its own, the on:do: and ensure: chains
	// start empty, and the home-token counter starts fresh -- a `^` out of a
	// block belongs to an activation on THIS stack or to none.
	memset(&CurrentThread, 0, sizeof(CurrentThread));
	CurrentThread.heap = work->heap;
	initRememberedSet(&CurrentThread.rememberedSet);
	heapAddMutator(work->heap, &CurrentThread);

	HandleScope scope;
	openHandleScope(&scope);
	// Read the array THROUGH the handle, now, rather than having been handed an
	// element: every worker that started before this one has been allocating.
	Value block = rawObjectIndexedPointers(work->blocks->raw)[work->index];
	_Bool understood = 0;
	Value result = jitSendUnary(block, "value", &understood);

	if (understood) {
		// Through the generational barrier, because the results Array may be old
		// by now and a block's answer is freshly allocated more often than not.
		// heapEndMutator splices what this logs into the heap's own set, so the
		// edge is not lost when this thread's storage goes away.
		//
		// `results` is re-read here and not hoisted above the send: the send
		// allocates, so a body pointer taken before it would point into the
		// evacuated semispace.
		RawObject *results = work->results->raw;
		rawObjectStoreValue(results,
			&rawObjectIndexedPointers(results)[work->index], result);
		work->answered = 1;
	}
	closeHandleScope(&scope, NULL);

	heapEndMutator(work->heap, &CurrentThread);
}


Value primWorkerParallel(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value blocksValue = primitiveArgument(args, 0);
	if (!valueTypeOf(blocksValue, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *blocksRaw = asObject(blocksValue);
	if (rawObjectFormat(blocksRaw) != FORMAT_INDEXED_POINTERS) {
		return PRIMITIVE_FAILED;
	}
	size_t count = rawObjectElementCount(blocksRaw);

	// EVERY element is checked BEFORE a single thread starts, and the whole
	// primitive fails if one is not a block. The alternative is discovering it on
	// a worker, where there is no longer a way to fail: the primitive has already
	// answered. Failing here runs the kernel's own `self error:`, which names the
	// problem on the caller's stack where it can be read.
	for (size_t i = 0; i < count; i++) {
		if (!isClosure(rawObjectIndexedPointers(blocksRaw)[i])) {
			return PRIMITIVE_FAILED;
		}
	}

	// From here on this function allocates and other threads collect, so the
	// caller's frame is anchored until the very last join. See the header.
	PRIMITIVE_ALLOCATES(args);

	HandleScope scope;
	openHandleScope(&scope);
	Object *blocks = scopeHandle(asObject(blocksValue));
	Object *results = (Object *) newArray(count);

	// No blocks is not a special case worth a thread: an empty Array is the right
	// answer and spawning nothing is the right way to produce it.
	if (count == 0) {
		Value answer = objectTagged(results);
		closeHandleScope(&scope, NULL);
		PRIMITIVE_DONE_ALLOCATING();
		return answer;
	}

	ParallelWork *work = calloc(count, sizeof(ParallelWork));
	if (work == NULL) {
		closeHandleScope(&scope, NULL);
		PRIMITIVE_DONE_ALLOCATING();
		return PRIMITIVE_FAILED;
	}
	Heap *heap = CurrentThread.heap;
	for (size_t i = 0; i < count; i++) {
		work[i].blocks = blocks;
		work[i].results = results;
		work[i].index = i;
		work[i].heap = heap;
	}

	// Blocked BEFORE the first spawn, because worker zero can request the world
	// the instant it starts, and left blocked until the last join for the same
	// reason. See the header for why this is not the same obligation as the
	// anchor above.
	heapGcEnterBlocked(heap, &CurrentThread);
	size_t spawned = 0;
	while (spawned < count
			&& osThreadSpawn(&work[spawned].thread, parallelWorkerBody,
				&work[spawned])) {
		spawned++;
	}
	// Join exactly what was spawned. Joining a thread that never started is
	// undefined, and the count is the only thing that knows which those are.
	for (size_t i = 0; i < spawned; i++) {
		osThreadJoin(&work[i].thread);
	}
	heapGcLeaveBlocked(heap, &CurrentThread);

	// A spawn that failed is reported as failure of the whole send rather than as
	// an Array with holes in it: the kernel promises result i is block i's answer,
	// and a nil that means "the OS refused a thread" is indistinguishable from a
	// block that answered nil.
	_Bool complete = spawned == count;
	for (size_t i = 0; i < spawned; i++) {
		complete = complete && work[i].answered;
	}
	free(work);

	if (!complete) {
		closeHandleScope(&scope, NULL);
		PRIMITIVE_DONE_ALLOCATING();
		return PRIMITIVE_FAILED;
	}
	Value answer = objectTagged(results);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
