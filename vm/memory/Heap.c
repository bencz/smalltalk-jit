#include "memory/Heap.h"
#include "memory/Collector.h"
#include "memory/ObjectWalk.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include "core/Thread.h"
#include "os/Os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB 1024
#define MB (1024 * 1024)

#define NURSERY_BYTES (32 * MB)
#define OLD_PAGE_BYTES (256 * KB)
#define EXEC_PAGE_BYTES (256 * KB)
#define TLAB_BYTES (64 * KB)

// A full collection runs only once the old space grows past a threshold that
// itself grows with the live set. Without that, a workload that steadily
// promotes triggers a full collection on nearly every young one, which is
// O(N) full collections and therefore O(N^2).
#define OLD_GC_MIN_THRESHOLD (16 * MB)
#define OLD_GC_GROWTH 2

PER_ISOLATE GCStats LastGCStats;

static PER_ISOLATE size_t gCodegenDepth;
static PER_ISOLATE size_t gSymbolDepth;


void initHeap(Heap *heap, struct Thread *thread)
{
	heap->thread = thread;
	heap->handles = calloc(1, sizeof(SmalltalkHandles)); // populated by bootstrap
	classTableInit(&heap->classes);
	initNursery(&heap->newSpace, heap, NURSERY_BYTES);
	initPageSpace(&heap->oldSpace, OLD_PAGE_BYTES, 0);
	initPageSpace(&heap->execSpace, EXEC_PAGE_BYTES, 1);
	heap->oldGcThreshold = OLD_GC_MIN_THRESHOLD;
	heap->gcEpoch = 0;

	osMutexInit(&heap->youngLock);
	osMutexInit(&heap->oldLock);
	osMutexInit(&heap->execLock);
	osMutexInit(&heap->codegenLock);
	osMutexInit(&heap->symbolLock);
	osMutexInit(&heap->gcLock);
	osMutexInit(&heap->safepointLock);
	osCondInit(&heap->safepointCond);
	heap->safepointRequested = 0;

	// Striped sync monitor: N = ST_MONITOR_STRIPES (default 64), clamped to a
	// power of two in [1, 4096]. N = 1 is the single-lock escape hatch.
	{
		size_t requested = 64;
		const char *env = getenv("ST_MONITOR_STRIPES");
		if (env != NULL && *env != '\0') {
			long v = strtol(env, NULL, 10);
			if (v >= 1) {
				requested = (size_t) (v > 4096 ? 4096 : v);
			}
		}
		size_t n = 1;
		unsigned log2n = 0;
		while (n * 2 <= requested) {
			n *= 2;
			log2n++;
		}
		heap->monitorStripeCount = n;
		heap->monitorStripeShift = (n <= 1) ? 0 : (32 - log2n);
		heap->monitorLocks = malloc(n * sizeof(OsMutex));
		ASSERT(heap->monitorLocks != NULL);
		for (size_t i = 0; i < n; i++) {
			osMutexInit(&heap->monitorLocks[i]);
		}
	}

	heap->symbolCount = 0;
	heap->symbolCountValid = 0;
	heap->mutators = NULL;
	heap->sched = NULL;
}


void freeHeap(Heap *heap)
{
	freeNursery(&heap->newSpace);
	freePageSpace(&heap->oldSpace);
	freePageSpace(&heap->execSpace);
	classTableFree(&heap->classes);
	for (size_t i = 0; i < heap->monitorStripeCount; i++) {
		osMutexDestroy(&heap->monitorLocks[i]);
	}
	free(heap->monitorLocks);
	heap->monitorLocks = NULL;
	free(heap->handles);
	heap->handles = NULL;
}


// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static uint8_t *allocateOld(Heap *heap, size_t bytes)
{
	osMutexLock(&heap->oldLock);
	uint8_t *address = pageSpaceAllocate(&heap->oldSpace, bytes);
	_Bool overThreshold = heap->oldSpace.allocated > heap->oldGcThreshold
		&& !heap->gcInhibited;
	osMutexUnlock(&heap->oldLock);
	if (address == NULL) {
		fprintf(stderr, "out of memory: old space refused %zu bytes\n", bytes);
		abort();
	}
	if (overThreshold) {
		collectorMarkSweep(heap);
		osMutexLock(&heap->oldLock);
		size_t live = heap->oldSpace.allocated;
		heap->oldGcThreshold = live * OLD_GC_GROWTH;
		if (heap->oldGcThreshold < OLD_GC_MIN_THRESHOLD) {
			heap->oldGcThreshold = OLD_GC_MIN_THRESHOLD;
		}
		osMutexUnlock(&heap->oldLock);
	}
	return address;
}


// Retire a TLAB: fill its unused tail with a free chunk and DROP it, so the
// owning mutator's next allocation refills from the current young space.
//
// Both halves matter and for different reasons. The filling is so that a linear
// walk, which strides on object headers, does not read the uncarved tail as
// garbage. The DROPPING is what a collection depends on: a TLAB is a pointer
// INTO one semispace, and a young collection swaps which semispace mutators
// allocate from. A TLAB that survived a collection would keep handing out
// addresses in the space that just became the next collection's DESTINATION,
// and the next collection would then copy survivors straight over live objects.
//
// That is not hypothetical. It is exactly what the memory self-test caught:
// 32 objects silently sharing addresses with 32 others, discovered because two
// handles that had never met came back pointing at the same word.
static void retireTlab(Thread *thread)
{
	size_t left = (size_t) (thread->tlab.end - thread->tlab.top);
	if (left >= HEAP_OBJECT_ALIGN) {
		freeChunkInit((RawObject *) thread->tlab.top, left);
	}
	thread->tlab.top = NULL;
	thread->tlab.end = NULL;
}


void heapFillAllTlabTails(Heap *heap)
{
	for (Thread *t = heap->mutators; t != NULL; t = t->nextMutator) {
		if (t->tlab.top != NULL) {
			retireTlab(t);
		}
	}
}


static _Bool refillTlab(Heap *heap, Thread *thread)
{
	osMutexLock(&heap->youngLock);
	uint8_t *chunk = nurseryCarve(&heap->newSpace, TLAB_BYTES);
	osMutexUnlock(&heap->youngLock);
	if (chunk == NULL) {
		return 0;
	}
	thread->tlab.top = chunk;
	thread->tlab.end = chunk + TLAB_BYTES;
	return 1;
}


uint8_t *allocate(Heap *heap, size_t bytes)
{
	bytes = objectAlignSize(bytes);
	// An object whose size does not fit the header's 8-bit field never enters
	// the nursery. It would otherwise need its size in a word BEFORE the header,
	// and a copying collector would have to carry that word along on every
	// evacuation. Sending such objects straight to the non-moving old space
	// removes that case from the copy path entirely. The cost is that a large
	// array is old from birth and only a full collection can reclaim it, which
	// is a policy worth measuring once there is something to measure.
	if (bytes > SIZE_INLINE_MAX_BYTES) {
		return allocateOld(heap, bytes);
	}

	Thread *thread = &CurrentThread;
	if (thread->tlab.top != NULL
			&& (size_t) (thread->tlab.end - thread->tlab.top) >= bytes) {
		uint8_t *address = thread->tlab.top;
		thread->tlab.top += bytes;
		return address;
	}

	if (thread->tlab.top != NULL) {
		retireTlab(thread);
	}
	if (refillTlab(heap, thread)) {
		uint8_t *address = thread->tlab.top;
		thread->tlab.top += bytes;
		return address;
	}

	// Young space exhausted: collect, then try once more. The collection
	// retires every TLAB itself, including this thread's.
	collectorScavenge(heap);
	if (refillTlab(heap, thread)) {
		uint8_t *address = thread->tlab.top;
		thread->tlab.top += bytes;
		return address;
	}
	// A nursery that cannot host a TLAB even after a collection means the live
	// young set fills it. Fall back to old space rather than loop.
	return allocateOld(heap, bytes);
}


// 22 bits of identity hash, derived from the address at birth. Objects move, so
// this is a birth-time value that then travels WITH the object, exactly like
// the old 32-bit field: identity hash must be stable across collections.
static uint32_t birthHash(void *address)
{
	uintptr_t bits = (uintptr_t) address >> 4;
	bits ^= bits >> 22;
	return (uint32_t) (bits & OBJ_HASH_MASK);
}


// Shared by the ordinary and the immortal allocator: everything except WHERE
// the bytes come from is identical, and a second copy of the header stamping is
// how the two would drift.
//
// EVERY BODY WORD IS WRITTEN HERE, and that is load-bearing rather than tidy.
// The semispaces are not re-zeroed when they flip and a swept old-space chunk
// keeps whatever it held, so from the second cycle onward an allocation lands on
// a DEAD OBJECT'S BYTES. A slot the allocator leaves alone therefore holds a
// value from that dead object, and the collector scans it: a garbage class index
// at best, a plausible pointer into the evacuated semispace at worst. The
// ALIGNMENT PADDING is covered for the same reason, because the pointer range
// comes from the object's SIZE and no caller ever writes padding.
//
// ZERO and not nil, which is tagInt(0) and therefore a legal Value the collector
// steps over. That is deliberate: throughout the VM an unset slot answers
// `valueTypeOf(slot, VALUE_POINTER)` as false and means ABSENT (a class with no
// superclass, a method with no owner), and nil is an object, so filling with it
// here would turn every one of those into "present, and it is nil". What
// Smalltalk requires instead, that `Object new` answers an instance whose
// variables are nil, belongs to the primitive that serves `new` and is done
// there (runtime/Primitive.c), where nil is guaranteed to exist.
// The class is passed as its SHAPE and its INDEX rather than as a pointer,
// because the allocation that precedes this can collect and move the class
// object: everything needed out of it is read before the bytes are asked for.
static RawObject *initializeObject(Heap *heap, RawObject *object,
	InstanceShape shape, uint32_t classIndex, size_t bytes, size_t elements)
{
	object->header = makeObjectHeader(classIndex, birthHash(object),
		(ObjectFormat) shape.format, bytes / sizeof(uint64_t));
	memset(object->body, 0, bytes - HEADER_SIZE);
	switch ((ObjectFormat) shape.format) {
	case FORMAT_INDEXED_POINTERS:
	case FORMAT_BYTES:
	case FORMAT_DOUBLES:
		// After the zeroing, and before the pointer range is computed: an object
		// too large for the header's size field answers its size FROM this count.
		rawObjectSetElementCount(object, elements);
		break;
	default:
		break;
	}
	(void) heap;
	return object;
}


RawObject *allocateImmortalObject(Heap *heap, RawObject *class, size_t elements)
{
	RawClass *raw = (RawClass *) class;
	size_t bytes = objectSizeForShape(raw->instanceShape, elements);
	// The OLD space, which never moves and never compacts (ADR 0005). Reachable
	// from the well-known handles, so the mark phase keeps it alive; what it
	// must never do is change address.
	RawObject *object = (RawObject *) allocateOld(heap, bytes);
	ASSERT(object != NULL);
	initializeObject(heap, object, raw->instanceShape, raw->classIndex, bytes,
		elements);
	ASSERT(isOldObject(object)); // the whole point: bit 3 says non-moving
	return object;
}


RawObject *allocateObject(Heap *heap, RawObject *class, size_t elements)
{
	RawClass *raw = (RawClass *) class;
	// Read out of the class BEFORE allocating: the allocation can collect, and a
	// collection moves the class.
	InstanceShape shape = raw->instanceShape;
	uint32_t classIndex = raw->classIndex;
	size_t bytes = objectSizeForShape(shape, elements);

	uint8_t *address = allocate(heap, bytes);
	return initializeObject(heap, (RawObject *) address, shape, classIndex, bytes,
		elements);
}


void collectGarbage(Heap *heap)
{
	// Not "skip it", an ASSERT. While an image is loading, the objects already
	// read are reachable ONLY from the loader's id-to-address table, which is a C
	// array the collector does not scan, so a collection here would sweep a
	// half-built image and the damage would surface far from the cause.
	ASSERT(!heap->gcInhibited);
	collectorMarkSweep(heap);
}


// Raw old-space bytes for the image loader, with no collection triggered.
//
// The OLD space and not the nursery, and that is correctness rather than
// convenience: the old space does not move (ADR 0005), so the loader's
// id-to-address table stays valid for the whole load. In the nursery a
// collection partway through would relocate everything already read and leave
// every entry stale.
uint8_t *heapAllocateImageBytes(Heap *heap, size_t bytes)
{
	ASSERT(heap->gcInhibited);
	return allocateOld(heap, bytes);
}


void heapInhibitGc(Heap *heap, _Bool inhibited)
{
	heap->gcInhibited = inhibited;
}


// ---------------------------------------------------------------------------
// Stop-the-world handshake
// ---------------------------------------------------------------------------

void heapGcPoll(Heap *heap, Thread *self)
{
	if (!__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
		return; // hot path: one acquire load
	}
	osMutexLock(&heap->safepointLock);
	if (__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
		self->spAtSafepoint = 1;
		osCondBroadcast(&heap->safepointCond);
		while (__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
			osCondWait(&heap->safepointCond, &heap->safepointLock);
		}
		self->spAtSafepoint = 0;
	}
	osMutexUnlock(&heap->safepointLock);
}


static int allSafe(Heap *heap, Thread *exclude)
{
	for (Thread *t = heap->mutators; t != NULL; t = t->nextMutator) {
		if (t == exclude) {
			continue;
		}
		if (!t->spAtSafepoint && !t->spBlocked) {
			return 0;
		}
	}
	return 1;
}


void heapGcBegin(Heap *heap, Thread *self)
{
	osMutexLock(&heap->safepointLock);
	__atomic_store_n(&heap->safepointRequested, 1, __ATOMIC_RELEASE);
	while (!allSafe(heap, self)) {
		osCondWait(&heap->safepointCond, &heap->safepointLock);
	}
	osMutexUnlock(&heap->safepointLock);
}


void heapGcEnd(Heap *heap, Thread *self)
{
	(void) self;
	osMutexLock(&heap->safepointLock);
	__atomic_store_n(&heap->safepointRequested, 0, __ATOMIC_RELEASE);
	osCondBroadcast(&heap->safepointCond);
	osMutexUnlock(&heap->safepointLock);
}


// A mutator about to block in a native wait declares itself safe: it holds no
// heap pointers the collector could not reach, so a peer must not wait for it.
void heapGcEnterBlocked(Heap *heap, Thread *self)
{
	osMutexLock(&heap->safepointLock);
	self->spBlocked = 1;
	osCondBroadcast(&heap->safepointCond);
	osMutexUnlock(&heap->safepointLock);
}


void heapGcLeaveBlocked(Heap *heap, Thread *self)
{
	osMutexLock(&heap->safepointLock);
	while (__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
		osCondWait(&heap->safepointCond, &heap->safepointLock);
	}
	self->spBlocked = 0;
	osMutexUnlock(&heap->safepointLock);
}


void heapAddMutator(Heap *heap, Thread *thread)
{
	// Under gcLock, which the collector holds for a whole collection, so a
	// worker can never join the mutator list while one is walking it.
	osMutexLock(&heap->gcLock);
	osMutexLock(&heap->youngLock);
	thread->nextMutator = heap->mutators;
	heap->mutators = thread;
	osMutexUnlock(&heap->youngLock);
	osMutexUnlock(&heap->gcLock);
}


void heapEndMutator(Heap *heap, Thread *thread)
{
	heapGcEnterBlocked(heap, thread); // waiting on gcLock counts as safe
	osMutexLock(&heap->gcLock);
	heapGcLeaveBlocked(heap, thread);
	osMutexLock(&heap->youngLock);
	if (thread->tlab.top != NULL) {
		retireTlab(thread);
	}
	Thread **link = &heap->mutators;
	while (*link != NULL && *link != thread) {
		link = &(*link)->nextMutator;
	}
	if (*link == thread) {
		*link = thread->nextMutator;
	}
	osMutexUnlock(&heap->youngLock);

	// Splice this worker's barrier log into the heap-level thread's, so the
	// old-to-young edges it recorded are not lost when its storage goes away.
	RememberedSetBlock *front = thread->rememberedSet.blocks;
	if (front != NULL && heap->thread != NULL && heap->thread != thread) {
		RememberedSetBlock *tail = front;
		while (tail->prev != NULL) {
			tail = tail->prev;
		}
		tail->prev = heap->thread->rememberedSet.blocks;
		heap->thread->rememberedSet.blocks = front;
		thread->rememberedSet.blocks = NULL;
	}
	osMutexUnlock(&heap->gcLock);
}


// ---------------------------------------------------------------------------
// Locks
// ---------------------------------------------------------------------------

void heapCodegenLockEnter(Heap *heap)
{
	// Re-entrant: generating a stub while compiling a method takes it twice.
	if (gCodegenDepth++ == 0) {
		heapGcEnterBlocked(heap, &CurrentThread); // waiting counts as GC-safe
		osMutexLock(&heap->codegenLock);
		heapGcLeaveBlocked(heap, &CurrentThread);
	}
}


void heapCodegenLockLeave(Heap *heap)
{
	if (--gCodegenDepth == 0) {
		osMutexUnlock(&heap->codegenLock);
	}
}


void heapSymbolLockEnter(Heap *heap)
{
	if (gSymbolDepth++ == 0) {
		heapGcEnterBlocked(heap, &CurrentThread);
		osMutexLock(&heap->symbolLock);
		heapGcLeaveBlocked(heap, &CurrentThread);
	}
}


void heapSymbolLockLeave(Heap *heap)
{
	if (--gSymbolDepth == 0) {
		osMutexUnlock(&heap->symbolLock);
	}
}


// Stripe for a synchronization object, derived from its identity hash. Computed
// ONCE at enter and stashed in the Thread; exit reads it back and never
// recomputes, so enter and exit provably drop the same lock even if the object
// moved or was become:-d inside the critical section.
size_t heapMonitorEnterStripe(Heap *heap, RawObject *object)
{
	size_t stripe = 0;
	if (heap->monitorStripeCount > 1) {
		uint32_t hash = rawObjectHash(object);
		stripe = (size_t) ((hash * 2654435761u) >> heap->monitorStripeShift)
			& (heap->monitorStripeCount - 1);
	}
	osMutexLock(&heap->monitorLocks[stripe]);
	return stripe;
}


void heapMonitorExitStripe(Heap *heap, size_t stripe)
{
	ASSERT(stripe < heap->monitorStripeCount);
	osMutexUnlock(&heap->monitorLocks[stripe]);
}


// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void resetGcStats(void)
{
	LastGCStats.count = 0;
	LastGCStats.marked = 0;
	LastGCStats.sweeped = 0;
	LastGCStats.freed = 0;
	LastGCStats.extended = 0;
	LastGCStats.total = 0;
	LastGCStats.time = 0;
	LastGCStats.totalTime = 0;
}


void printGcStats(void)
{
	printf("gc: %zu full, %zu young, %lld us young total, %zu bytes freed\n",
		LastGCStats.count, LastGCStats.scavengeCount,
		(long long) LastGCStats.scavengeTimeUs, LastGCStats.freed);
}


void printHeap(Heap *heap)
{
	printf("heap: young %zu/%zu bytes free, old %zu allocated of %zu mapped, "
		"exec %zu of %zu, %zu classes\n",
		nurseryAvailable(&heap->newSpace), heap->newSpace.semiSize,
		heap->oldSpace.allocated, heap->oldSpace.capacity,
		heap->execSpace.allocated, heap->execSpace.capacity,
		heap->classes.size);
}
