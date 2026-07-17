#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "memory/GarbageCollector.h"
#include "core/Assert.h"
#include "os/Os.h"
#include "core/CompiledCode.h"
#include "runtime/String.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

// From core/Lookup.c (not via Lookup.h: that header pulls CompiledCode/JIT
// types this TU does not need). Flushes the calling thread's TLS LookupCache
// when heap->gcEpoch moved past Thread.lookupCacheEpoch.
void lookupCacheOnGcResume(Thread *self);

#define KB 1024
#define MB (1024 * 1024)

#define SCAVENGE_EVERY_ALLOC 0
#define VERIFY_HEAP_AFTER_GC 0

// Full GC (mark/sweep) runs only once old-space capacity grows past a threshold
// that itself grows with the live set — instead of on every page growth. Without
// this, a workload that steadily promotes (e.g. 100k live fibers' contexts)
// triggers a full GC on nearly every scavenge = O(N) full GCs = O(N²).
#define OLD_GC_MIN_THRESHOLD (16 * MB)
#define OLD_GC_GROWTH 2

static void nilVars(Value *vars, size_t count);
static uint8_t *pageSpaceAllocate(PageSpace *pageSpace, size_t size);
static void emptyRememberedSet(void);
static void verifyObject(Heap *heap, RawObject *object);
static void verifyPointer(Heap *heap, RawObject *object);
static void printHeapPage(HeapPage *page);
static void printFreeSpace(FreeSpace *freeSpace);
static void printPageSpace(PageSpace *space);


void initHeap(Heap *heap, struct Thread *thread)
{
	heap->thread = thread;
	// Per-heap well-known handles (see Heap.h / the `Handles` macro). Zeroed; bootstrap
	// (or an isolate's own bootstrap) populates the fields. Shared by this heap's workers.
	heap->handles = calloc(1, sizeof(SmalltalkHandles));
	initScavenger(&heap->newSpace, heap, 64 * MB);
	initPageSpace(&heap->oldSpace, 256 * KB, 0);
	initPageSpace(&heap->execSpace, 256 * KB, 1);
	heap->oldGcThreshold = OLD_GC_MIN_THRESHOLD;
	pthread_mutex_init(&heap->youngLock, NULL);
	pthread_mutex_init(&heap->oldLock, NULL);
	pthread_mutex_init(&heap->execLock, NULL);
	pthread_mutex_init(&heap->codegenLock, NULL);
	pthread_mutex_init(&heap->monitorLock, NULL);
	pthread_mutex_init(&heap->symbolLock, NULL);
	heap->symbolCount = 0;
	heap->symbolCountValid = 0; // force a recount on first intern (snapshot restores the table only)
	pthread_mutex_init(&heap->gcLock, NULL);
	pthread_mutex_init(&heap->safepointLock, NULL);
	pthread_cond_init(&heap->safepointCond, NULL);
	heap->safepointRequested = 0;
	heap->gcEpoch = 0;
	heap->mutators = NULL;
	initRememberedSet(&heap->rememberedSet); // consolidated old->young set (survives worker exit)
	heap->sched = NULL; // allocated by schedulerInit, once per heap
	for (int i = 0; i < STUB_COUNT; i++) {
		heap->stubCode[i] = NULL; // JIT stubs generated lazily, once per heap
	}
}


// Register `thread` as a mutator of `heap` so the GC scans its roots. Taken under
// gcLock (which the collector holds for the whole collection) so a worker can NEVER
// join heap->mutators while a scavenge is walking that list / moving objects: the
// registering thread simply waits for any in-progress collection to finish. This
// mirrors heapEndMutator and closes the registration race (a worker that joined
// after the collector's heapGcBegin snapshot used to run concurrently with the
// scavenge, corrupting value slots). Registration is rare, so the extra lock is
// free in practice; youngLock still guards the actual list write.
void heapAddMutator(Heap *heap, struct Thread *thread)
{
	pthread_mutex_lock(&heap->gcLock);
	pthread_mutex_lock(&heap->youngLock);
	thread->nextMutator = heap->mutators;
	heap->mutators = thread;
	pthread_mutex_unlock(&heap->youngLock);
	pthread_mutex_unlock(&heap->gcLock);
}


// Unregister an exiting worker thread from `heap->mutators` so a later GC never
// scans the roots of a dead OS thread. Done under gcLock (which the collector
// holds while iterating the list), so it never races a scavenge.
void heapEndMutator(Heap *heap, Thread *thread)
{
	heapGcEnterBlocked(heap, thread); // waiting on gcLock counts as safe
	pthread_mutex_lock(&heap->gcLock);
	heapGcLeaveBlocked(heap, thread);
	pthread_mutex_lock(&heap->youngLock);
	Thread **link = &heap->mutators;
	while (*link != NULL && *link != thread) {
		link = &(*link)->nextMutator;
	}
	if (*link == thread) {
		*link = thread->nextMutator;
	}
	pthread_mutex_unlock(&heap->youngLock);

	// Splice this exiting worker's per-thread barrier delta (old->young edges recorded
	// since the last GC) into the heap-level consolidated set, so they are NOT lost when
	// the thread's TLS is reclaimed. Raw block-chain splice — NOT rememberedSetAdd: the
	// entries keep TAG_REMEMBERED (rememberedSetAdd ASSERTs it is clear). Safe under
	// gcLock (still held): the heap-level set is only ever touched under gcLock / STW.
	RememberedSetBlock *front = thread->rememberedSet.blocks;
	if (front != NULL) {
		_Bool empty = (front->prev == NULL) && (front->current == front->objects);
		if (empty) {
			rememberedSetFreeBlocks(front); // lone empty head — nothing to transfer
		} else {
			RememberedSetBlock *tail = front; // oldest block has prev == NULL
			while (tail->prev != NULL) {
				tail = tail->prev;
			}
			tail->prev = heap->rememberedSet.blocks; // concatenate: thread chain -> heap chain
			heap->rememberedSet.blocks = front;
		}
		thread->rememberedSet.blocks = NULL; // ownership transferred; thread is dying
	}

	pthread_mutex_unlock(&heap->gcLock);
}


// ---- stop-the-world safepoint handshake (per shared heap) ------------------
// Mirrors vm/Safepoint.c but scoped to ONE heap's mutators (heap->mutators), so
// a collection on the shared heap never stops threads of a different isolate.

void heapGcPoll(Heap *heap, Thread *self)
{
	if (!__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
		return; // hot path: one acquire load
	}
	pthread_mutex_lock(&heap->safepointLock);
	if (__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
		self->spAtSafepoint = 1;
		pthread_cond_broadcast(&heap->safepointCond); // wake a waiting collector
		while (__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
			pthread_cond_wait(&heap->safepointCond, &heap->safepointLock);
		}
		self->spAtSafepoint = 0;
	}
	pthread_mutex_unlock(&heap->safepointLock);
	lookupCacheOnGcResume(self); // the collection may have moved cached classes/selectors
}

static int heapAllSafe(Heap *heap, Thread *exclude)
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
	pthread_mutex_lock(&heap->safepointLock);
	__atomic_store_n(&heap->safepointRequested, 1, __ATOMIC_RELEASE);
	while (!heapAllSafe(heap, self)) {
		pthread_cond_wait(&heap->safepointCond, &heap->safepointLock);
	}
	pthread_mutex_unlock(&heap->safepointLock);
}

void heapGcEnd(Heap *heap)
{
	pthread_mutex_lock(&heap->safepointLock);
	__atomic_store_n(&heap->safepointRequested, 0, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&heap->safepointCond);
	pthread_mutex_unlock(&heap->safepointLock);
}

void heapGcEnterBlocked(Heap *heap, Thread *self)
{
	pthread_mutex_lock(&heap->safepointLock);
	self->spBlocked = 1;
	pthread_cond_broadcast(&heap->safepointCond);
	pthread_mutex_unlock(&heap->safepointLock);
}

void heapGcLeaveBlocked(Heap *heap, Thread *self)
{
	pthread_mutex_lock(&heap->safepointLock);
	while (__atomic_load_n(&heap->safepointRequested, __ATOMIC_ACQUIRE)) {
		pthread_cond_wait(&heap->safepointCond, &heap->safepointLock);
	}
	self->spBlocked = 0;
	pthread_mutex_unlock(&heap->safepointLock);
	lookupCacheOnGcResume(self); // a collection may have run while blocked
}


// ---- JIT code-generation lock (serializes codegen across worker threads) ----
// Only ONE thread generates JIT code at a time. Held across a whole method/stub
// generation, which allocates young objects and may scavenge — so acquisition uses
// the "waiting counts as safe" pattern (enter the blocked state before blocking on
// the lock) to avoid deadlocking a peer collector. A per-thread depth counter makes
// it re-entrant (a stub is generated while a method is being compiled).
static PER_ISOLATE int gCodegenDepth = 0;

void heapCodegenLockEnter(Heap *heap)
{
	if (gCodegenDepth++ == 0) {
		heapGcEnterBlocked(heap, &CurrentThread); // waiting on codegenLock counts as safe
		pthread_mutex_lock(&heap->codegenLock);
		heapGcLeaveBlocked(heap, &CurrentThread);
	}
}

void heapCodegenLockLeave(Heap *heap)
{
	if (--gCodegenDepth == 0) {
		pthread_mutex_unlock(&heap->codegenLock);
	}
}


// ---- Smalltalk sync monitor (Semaphore/Channel/...) ----
// Acquisition mirrors heapCodegenLockEnter: a fiber holding the monitor may allocate
// (the critical sections do `waiting addLast:` etc.) and thus trigger a scavenge, and a
// fiber WAITING on the monitor must not stall the STW handshake — so it enters the
// blocked state before locking. Unlike codegenLock this is NOT re-entrant: the sync
// primitives keep their critical sections flat (never take the monitor while holding it).
void heapMonitorEnter(Heap *heap)
{
	heapGcEnterBlocked(heap, &CurrentThread); // waiting on monitorLock counts as safe
	pthread_mutex_lock(&heap->monitorLock);
	heapGcLeaveBlocked(heap, &CurrentThread);
}

void heapMonitorExit(Heap *heap)
{
	pthread_mutex_unlock(&heap->monitorLock);
}


// ---- symbol interning lock (asSymbol / growSymbolTable) ----
// Re-entrant (growSymbolTable re-enters asSymbol through setGlobal to keep the Smalltalk
// `SymbolTable` global in sync), with a per-thread depth counter like codegenLock. GC-safe
// acquisition: interning allocates (a new Symbol, or a 2x table) and can scavenge, and a
// thread waiting on this lock must not stall a peer's STW — so it enters the blocked state
// before locking. A leaf lock (never taken while holding it another lock that could invert).
static PER_ISOLATE int gSymbolDepth = 0;

void heapSymbolLockEnter(Heap *heap)
{
	if (gSymbolDepth++ == 0) {
		heapGcEnterBlocked(heap, &CurrentThread); // waiting on symbolLock counts as safe
		pthread_mutex_lock(&heap->symbolLock);
		heapGcLeaveBlocked(heap, &CurrentThread);
	}
}

void heapSymbolLockLeave(Heap *heap)
{
	if (--gSymbolDepth == 0) {
		pthread_mutex_unlock(&heap->symbolLock);
	}
}


void freeHeap(Heap *heap)
{
	freeScavenger(&heap->newSpace);
	freePageSpace(&heap->oldSpace);
	freePageSpace(&heap->execSpace);
	rememberedSetFreeBlocks(heap->rememberedSet.blocks); // free the whole chain (incl. head)
	heap->rememberedSet.blocks = NULL;
	free(heap->handles);
	heap->handles = NULL;
}


RawObject *allocateObject(Heap *heap, RawClass *class, size_t size)
{
	HandleScope scope;
	openHandleScope(&scope);

	InstanceShape shape = class->instanceShape;
	size_t realSize = computeInstanceSize(class->instanceShape, size);
	Class *classHandle = scopeHandle(class);
#if SCAVENGE_EVERY_ALLOC
	scavengerScavenge(&heap->newSpace);
#endif
	RawObject *object = (RawObject *) allocate(heap, realSize);

	object->class = classHandle->raw;
	object->hash = (Value) object >> 2; // XXX: replace with random hash generator
	object->payloadSize = shape.payloadSize;
	object->varsSize = shape.varsSize;
	object->tags = 0;

	// An exhausted young space sends this allocation to OLD space (allocate()'s
	// tryAllocateOld fallback). C-side creators then initialize such an object with
	// RAW stores that pass through no write barrier: this class word (young while a
	// snapshot-loaded class is still un-promoted), copyResizedObject's field memcpy,
	// primitive init loops. Any young referent stored that way is an UNTRACKED
	// old->young edge — the next scavenge moves the referent and leaves this
	// object's word dangling (seen as ST_PARFULLGC's corrupted-kernel-class family).
	// Conservatively remember every old-space birth; the next scavenge re-scans it
	// and drops it from the set again if it holds no young referent after all.
	if (isOldObject(object)) {
		rememberedSetAdd(&CurrentThread.rememberedSet, object);
	}

	if (shape.isIndexed) {
		((RawIndexedObject *) object)->size = size;
		memset(((RawIndexedObject *) object)->body, 0, shape.payloadSize * sizeof(Value));
	} else {
		memset(object->body, 0, shape.payloadSize * sizeof(Value));
	}
	if (shape.isBytes) {
		nilVars(getRawObjectVars(object), class->instanceShape.varsSize);
		memset(getRawObjectIndexedVars(object), 0, size);
	} else {
		nilVars(getRawObjectVars(object), class->instanceShape.varsSize + size);
	}
	closeHandleScope(&scope, NULL);
	return object;
}


static void nilVars(Value *vars, size_t count)
{
	Value nil = getTaggedPtr(Handles.nil);
	Value *var = vars;
	Value *end = vars + count;

	for (; var < end; var++) {
		*var = nil;
	}
}


void freeObject(PageSpace *space, RawObject *object)
{
	FreeSpace *freeSpace = createFreeSpace((uint8_t *) object, align(computeRawObjectSize(object), HEAP_OBJECT_ALIGN));
	freeListAddFreeSpace(&space->freeList, freeSpace);
}


NativeCode *allocateNativeCode(Heap *heap, size_t size, size_t pointersOffsetsSize)
{
	// Room for the inline offsets array at its 4-byte aligned base (the code
	// byte count has arbitrary parity): mirror nativeCodePointersOffsets.
	size_t realSize = align(sizeof(NativeCode) + ((size + 3) & ~(size_t) 3)
		+ pointersOffsetsSize * sizeof(uint32_t), HEAP_OBJECT_ALIGN);
	// Serialize concurrent exec-space carving across worker threads (see execLock).
	pthread_mutex_lock(&heap->execLock);
	NativeCode *code = (NativeCode *) pageSpaceAllocate(&heap->execSpace, realSize);
	pthread_mutex_unlock(&heap->execLock);
	code->size = size;
	code->pointersOffsetsSize = pointersOffsetsSize;
	code->tags = 0;
	return code;
}


static uint8_t *pageSpaceAllocate(PageSpace *pageSpace, size_t size)
{
	uint8_t *p = pageSpaceTryAllocate(pageSpace, size);
	if (p == NULL) {
		/* Grow by a fresh page. Pages are 256 KB by default, but a single
		 * object can be larger than that (e.g. a big Array being promoted to
		 * old space), so map a page large enough to actually hold it. */
		size_t pageSize = 256 * KB;
		size_t needed = size + sizeof(HeapPage) + HEAP_OBJECT_ALIGN;
		if (needed > pageSize) {
			pageSize = needed;
		}
		HeapPage *page = mapHeapPage(pageSize, pageSpace->pagesTail->isExecutable);
		pageSpace->pagesTail->next = page;
		pageSpace->pagesTail = page;
		pageSpaceIndexAdd(pageSpace, page); // before the alloc below (its assert queries the index)
		expandFreeList(&pageSpace->freeList, page);
		p = pageSpaceTryAllocate(pageSpace, size);
		ASSERT(p != NULL);
		return p;
	}
	return p;
}


// How much young space a single TLAB refill carves. Small enough that several
// worker OS threads share one nursery; large enough to amortise the lock.
#define TLAB_CHUNK_BYTES (256 * KB)

// Write a filler over an abandoned TLAB tail so the nursery stays linearly
// parseable across the gap. Young addresses carry NEW_SPACE_TAG (so `p` is not
// 16-aligned) — write the FreeSpace header inline rather than via createFreeSpace
// (whose assert would reject the tagged pointer). Layout matches FreeSpace, so
// linear walkers detect TAG_FREESPACE (offset 15) and skip `size` bytes.
static void youngFill(uint8_t *p, size_t size)
{
	FreeSpace *filler = (FreeSpace *) p;
	filler->next = NULL;
	filler->size = size;
	filler->tags = TAG_FREESPACE;
}

// Retire EVERY mutator's live TLAB tail into a FREESPACE filler so the whole young
// space [fromSpace, newSpace.top) is linearly walkable. For become: under a
// stop-the-world safepoint: all peers are parked, so their tlab.top/end are stable, and
// a plain walker (swapObjectInNewSpace) can then skip the retired tails. Does NOT advance
// tlab.top — each mutator resumes bump-allocating from where it left off (overwriting the
// now-dead filler), so no TLAB space is lost.
void heapFillAllTlabTails(Heap *heap)
{
	for (struct Thread *m = heap->mutators; m != NULL; m = m->nextMutator) {
		size_t tail = (size_t) (m->tlab.end - m->tlab.top);
		if (tail >= HEAP_OBJECT_ALIGN) {
			youngFill(m->tlab.top, tail);
		}
	}
}

// Carve a fresh TLAB chunk from the shared young space for the CALLING OS thread.
// The per-mutator bump (fast path, in allocate()/the JIT) is lock-free; only this
// refill takes youngLock, so multiple workers can share one nursery. Returns 0 if
// the nursery lacks room for `realSize` (caller scavenges + retries).
static _Bool tlabRefill(Heap *heap, size_t realSize)
{
	pthread_mutex_lock(&heap->youngLock);
	Scavenger *s = &heap->newSpace;
	size_t available = (size_t) (s->end - s->top);
	if (available < realSize) {
		pthread_mutex_unlock(&heap->youngLock);
		return 0;
	}

	// Retire the tail of the current TLAB into a filler (only now, when we are
	// actually replacing it) so the abandoned bytes remain walkable.
	size_t tail = (size_t) (CurrentThread.tlab.end - CurrentThread.tlab.top);
	if (tail >= HEAP_OBJECT_ALIGN) {
		youngFill(CurrentThread.tlab.top, tail);
	}

	size_t chunk = TLAB_CHUNK_BYTES < realSize ? realSize : TLAB_CHUNK_BYTES;
	chunk = align(chunk, HEAP_OBJECT_ALIGN);
	if (chunk > available) {
		chunk = available;
	}
	// tlab.top carries NEW_SPACE_TAG (inherited from s->top); free = end - top.
	CurrentThread.tlab.top = s->top;
	CurrentThread.tlab.end = s->top + chunk;
	s->top += chunk;
	pthread_mutex_unlock(&heap->youngLock);
	return 1;
}


// Run mark/sweep after a scavenge if old space grew past its (self-raising)
// threshold. Factored out so both the single- and multi-mutator collection paths
// share exactly one copy.
static void maybeFullGc(Heap *heap)
{
	if (heap->newSpace.hasPromotionFailure && heap->oldSpace.totalBytes >= heap->oldGcThreshold) {
		markAndSweep(&CurrentThread);
		heap->oldGcThreshold = heap->oldSpace.totalBytes * OLD_GC_GROWTH;
		if (heap->oldGcThreshold < OLD_GC_MIN_THRESHOLD) {
			heap->oldGcThreshold = OLD_GC_MIN_THRESHOLD;
		}
	}
}

// Reclaim young space. Always taken under gcLock, so it is serialized with mutator
// registration/deregistration (heapAddMutator/heapEndMutator also hold gcLock) and
// with any peer collector. Deciding single- vs multi-mutator INSIDE the lock is
// what makes it race-free: a worker cannot slip into heap->mutators between the
// count check and the scavenge. Single mutator (main alone, or an isolate) collects
// directly (no stop-the-world needed — it is the only mutator, and registration is
// blocked on gcLock); several mutators coordinate a stop-the-world safepoint (park
// every other mutator at a poll) so a scavenge never moves objects under a runner.
static void heapCollectYoung(Heap *heap, size_t realSize)
{
	heapGcEnterBlocked(heap, &CurrentThread); // blocking on gcLock counts as safe
	pthread_mutex_lock(&heap->gcLock);
	heapGcLeaveBlocked(heap, &CurrentThread);

	if (heap->mutators == NULL || heap->mutators->nextMutator == NULL) {
		scavengerScavenge(&heap->newSpace);
		maybeFullGc(heap);
		heap->gcEpoch++;
		lookupCacheOnGcResume(&CurrentThread);
	} else {
		// A peer may have freed space while we waited for gcLock; only collect if
		// still needed. Read newSpace.top under youngLock — the lock that guards the
		// bump cursor a peer's tlabRefill advances — so this re-check does not race
		// the carve (a plain read here is a benign but real data race, per TSan).
		pthread_mutex_lock(&heap->youngLock);
		_Bool needed = (size_t) (heap->newSpace.end - heap->newSpace.top) < realSize;
		pthread_mutex_unlock(&heap->youngLock);
		if (needed) {
			heapGcBegin(heap, &CurrentThread); // park every other mutator
			scavengerScavenge(&heap->newSpace);
			maybeFullGc(heap);
			heap->gcEpoch++; // peers flush their lookup caches on resume
			heapGcEnd(heap);
			lookupCacheOnGcResume(&CurrentThread);
		}
	}
	pthread_mutex_unlock(&heap->gcLock);
}


uint8_t *allocate(Heap *heap, size_t size)
{
	size_t realSize = align(size, HEAP_OBJECT_ALIGN);
	TLAB *tlab = &CurrentThread.tlab;

	// Fast path: bump the TLAB (also inlined by the JIT AllocateStub).
	if ((size_t) (tlab->end - tlab->top) >= realSize) {
		uint8_t *p = tlab->top;
		tlab->top += realSize;
		return p;
	}

	// Slow path = a GC safepoint poll point: if a peer mutator is collecting this
	// shared heap, park here until it finishes (our TLAB is reset by its scavenge).
	heapGcPoll(heap, &CurrentThread);

	if (tlabRefill(heap, realSize)) {
		uint8_t *p = tlab->top;
		tlab->top += realSize;
		return p;
	}

	heapCollectYoung(heap, realSize);

	if (tlabRefill(heap, realSize)) {
		uint8_t *p = tlab->top;
		tlab->top += realSize;
		return p;
	}
	return tryAllocateOld(heap, realSize, 1);
}


uint8_t *tryAllocateOld(Heap *heap, size_t size, _Bool grow)
{
	size_t realSize = align(size, HEAP_OBJECT_ALIGN);
	pthread_mutex_lock(&heap->oldLock);
	uint8_t *p = pageSpaceTryAllocate(&heap->oldSpace, realSize);
	if (p == NULL && grow) {
		p = pageSpaceAllocate(&heap->oldSpace, realSize);
	}
	pthread_mutex_unlock(&heap->oldLock);
	ASSERT(p == NULL || isOldObject((RawObject *) p));
	return p;
}


void collectGarbage(Thread *thread)
{
	scavengerScavenge(&thread->heap->newSpace);
	markAndSweep(thread);
}


// Self-test observability (ST_PARFULLGC_TEST): count of full GCs run on any heap
// across all OS threads, so a test can confirm the multi-mutator markAndSweep path
// actually fired. Relaxed atomic — a diagnostic counter, never a synchronising fence.
unsigned long gFullGcRuns = 0;

void markAndSweep(Thread *thread)
{
	resetGcStats();
	LastGCStats.count++;
	__atomic_add_fetch(&gFullGcRuns, 1, __ATOMIC_RELAXED);
	int64_t startTime = osCurrentMicroTime();

	// Reset EVERY mutator's remembered set AND the heap-level consolidated set: they are
	// all parked at this stop-the-world safepoint, and marking rebuilds all surviving
	// old->young edges into the heap-level set (heap->rememberedSet), which the next
	// scavenge reads. Leaving any set intact would keep stale (possibly swept) old
	// objects in it -> next scavenge would walk freed memory.
	for (Thread *m = thread->heap->mutators; m != NULL; m = m->nextMutator) {
		rememberedSetReset(&m->rememberedSet);
	}
	rememberedSetReset(&thread->heap->rememberedSet);
	gcMarkRoots(thread);
	gcSweep(&thread->heap->oldSpace);

	LastGCStats.time = osCurrentMicroTime() - startTime;
	LastGCStats.totalTime += LastGCStats.time;

#if VERIFY_HEAP_AFTER_GC
	verifyHeap(thread->heap);
#endif
}


void verifyHeap(Heap *heap)
{
	RawObject *object = (RawObject *) ((uintptr_t) heap->newSpace.fromSpace | NEW_SPACE_TAG);

	// The young high-water is the TLAB cursor (newSpace.top was advanced when the
	// TLAB carved its chunk, so it no longer marks the last live object).
	while ((uint8_t *) object < CurrentThread.tlab.top) {
		if ((object->tags & TAG_FREESPACE) != 0) { // retired TLAB tail
			object = (RawObject *) ((uint8_t *) object + ((FreeSpace *) object)->size);
			continue;
		}
		verifyObject(heap, object);
		object = (RawObject *) ((uint8_t *) object + align(computeRawObjectSize(object), HEAP_OBJECT_ALIGN));
	}

	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &heap->oldSpace);
	object = pageSpaceIteratorNext(&iterator);

	while (object != NULL) {
		if ((object->tags & TAG_FREESPACE) == 0) {
			verifyObject(heap, object);
		}
		object = pageSpaceIteratorNext(&iterator);
	}
}


static void verifyObject(Heap *heap, RawObject *object)
{
	verifyPointer(heap, (RawObject *) object->class);

	Value *vars = getRawObjectVars(object);
	size_t size = object->class->instanceShape.varsSize;
	if (object->class->instanceShape.isIndexed && !object->class->instanceShape.isBytes) {
		size += rawObjectSize(object);
	}

	for (size_t i = 0; i < size; i++) {
		if (valueTypeOf(vars[i], VALUE_POINTER)) {
			verifyPointer(heap, asObject(vars[i]));
		}
	}
}


static void verifyPointer(Heap *heap, RawObject *object)
{
	if (scavengerIncludes(&heap->newSpace, (uint8_t *) object)) {
		return;
	}
	if (pageSpaceIncludes(&heap->oldSpace, (uint8_t *) object)) {
		return;
	}
	FAIL();
}


void printHeap(Heap *heap)
{
	printf("Scavenger\n\t");
	printHeapPage(heap->newSpace.page);
	printf("\tfree space: %zi\n", CurrentThread.tlab.top - heap->newSpace.fromSpace);

	printf("Old space\n");
	printPageSpace(&heap->oldSpace);

	printf("Executable space\n");
	printPageSpace(&heap->execSpace);
}


static void printHeapPage(HeapPage *page)
{
    printf("page %p size %zi%s\n", (void *)page, page->size, page->isExecutable ? " executable" : "");
}

static void printFreeSpace(FreeSpace *freeSpace)
{
    printf("free space %p size %llu\n", (void *)freeSpace, (unsigned long long)freeSpace->size);
}


static void printPageSpace(PageSpace *space)
{
	/*for (HeapPage *page = space->pages; page != NULL; page = page->next) {
		printf("\t");
		printHeapPage(page);
	}
	for (FreeSpace *freeSpace = space->spaces; freeSpace != NULL; freeSpace = freeSpace->next) {
		printf("\t");
		printFreeSpace(freeSpace);
	}*/
}


// ---- concurrent shared-heap allocation self-test (ST_TLAB_TEST=1) ----------
// N OS threads allocate from ONE shared young space at the same time, each via
// the real allocate() path (lock-free TLAB bump + youngLock-guarded refill), and
// stamp a unique 64-bit value into every object they are handed. If the locked
// carve ever handed two threads overlapping memory, a stamp would be clobbered.
// Sized to stay within the nursery so no scavenge/old-space fallback is hit.

#define TLAB_TEST_WORKERS 8
#define TLAB_TEST_ALLOCS  20000
#define TLAB_TEST_OBJSIZE 48

static Heap gTlabTestHeap;

typedef struct {
	long id;
	uint8_t **ptrs;
} TlabTestArg;

static void *tlabTestWorker(void *arg)
{
	TlabTestArg *ta = arg;
	// A fresh thread-local mutator that SHARES the one test heap.
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = &gTlabTestHeap;
	for (int i = 0; i < TLAB_TEST_ALLOCS; i++) {
		// Alternate young (TLAB carve under youngLock) and old (free list under
		// oldLock) so both shared allocators are hammered concurrently.
		uint8_t *p = (i & 1)
			? tryAllocateOld(&gTlabTestHeap, TLAB_TEST_OBJSIZE, 1)
			: allocate(&gTlabTestHeap, TLAB_TEST_OBJSIZE);
		*(uint64_t *) p = ((uint64_t) ta->id << 32) | (uint64_t) i; // stamp
		ta->ptrs[i] = p;
	}
	return NULL;
}

int tlabConcurrencySelfTest(void)
{
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = &gTlabTestHeap;
	initHeap(&gTlabTestHeap, &CurrentThread);

	pthread_t threads[TLAB_TEST_WORKERS];
	TlabTestArg args[TLAB_TEST_WORKERS];
	for (long w = 0; w < TLAB_TEST_WORKERS; w++) {
		args[w].id = w;
		args[w].ptrs = malloc(TLAB_TEST_ALLOCS * sizeof(uint8_t *));
		pthread_create(&threads[w], NULL, tlabTestWorker, &args[w]);
	}
	for (int w = 0; w < TLAB_TEST_WORKERS; w++) {
		pthread_join(threads[w], NULL);
	}

	long clobbered = 0;
	for (long w = 0; w < TLAB_TEST_WORKERS; w++) {
		for (int i = 0; i < TLAB_TEST_ALLOCS; i++) {
			uint64_t expect = ((uint64_t) w << 32) | (uint64_t) i;
			if (*(uint64_t *) args[w].ptrs[i] != expect) {
				clobbered++;
			}
		}
		free(args[w].ptrs);
	}

	long total = (long) TLAB_TEST_WORKERS * TLAB_TEST_ALLOCS;
	fprintf(stderr, "shared-heap alloc self-test: %d threads x %d allocs = %ld objects (young+old) on ONE shared heap, clobbered=%ld -> %s\n",
		TLAB_TEST_WORKERS, TLAB_TEST_ALLOCS, total, clobbered, clobbered == 0 ? "PASS" : "FAIL");
	return clobbered == 0 ? 0 : 1;
}
