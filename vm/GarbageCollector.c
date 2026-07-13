#include "GarbageCollector.h"
#include "Heap.h"
#include "Object.h"
#include "Handle.h"
#include "Class.h"
#include "CodeDescriptors.h"
#include "Entry.h"
#include "StackFrame.h"
#include "Thread.h"
#include "Exception.h"
#include "Scheduler.h"
#include "Fiber.h"
#include <stdlib.h>
#include <sys/mman.h>
#include "Assert.h"
#include <stdio.h>
#include <inttypes.h>

#define QUEUE_INIT_SIZE 1024

typedef struct {
	size_t size;
	RawObject **objects;
	ptrdiff_t index;
} MarkingQueue;

static void iterateStack(MarkingQueue *queue, Thread *thread);
static void iterateFiberStack(MarkingQueue *queue, Thread *thread, Fiber *fiber);
static void iterateMutatorCurrentStack(MarkingQueue *queue, Thread *thread, Thread *m);
static void iterateHandles(MarkingQueue *queue, Thread *thread);
static void iterateFiberHandles(MarkingQueue *queue, Thread *thread, Fiber *fiber);
static void iterateMutatorCurrentHandles(MarkingQueue *queue, Thread *thread, Thread *m);
static void iterateNativeCode(MarkingQueue *queue, Thread *thread);
static void iterateObject(MarkingQueue *queue, Thread *thread, RawObject *root);
static void markObject(MarkingQueue *queue, Thread *thread, RawObject *object);
static void markingQueueAdd(MarkingQueue *queue, RawObject *object);
static _Bool markingQueueIsEmpty(MarkingQueue *queue);
static RawObject *markingQueuePop(MarkingQueue *queue);
static _Bool hasFinalizer(RawObject *object);

PER_ISOLATE GCStats LastGCStats = { 0 };


void gcMarkRoots(Thread *thread)
{
	MarkingQueue queue = {
		.size = QUEUE_INIT_SIZE,
		.objects = malloc(QUEUE_INIT_SIZE * sizeof(RawObject *)),
		.index = 0,
	};
	iterateStack(&queue, thread);
	iterateHandles(&queue, thread);
	iterateNativeCode(&queue, thread);

	while (!markingQueueIsEmpty(&queue)) {
		iterateObject(&queue, thread, markingQueuePop(&queue));
	}

	free(queue.objects);
}


static void iterateExceptionHandlerChain(MarkingQueue *queue, Thread *thread, Value handlerValue)
{
	while (handlerValue != 0) {
		RawExceptionHandler *handler = (RawExceptionHandler *) asObject(handlerValue);
		markObject(queue, thread, (RawObject *) handler);
		handlerValue = handler->parent;
	}
}


static void iterateStackFrames(MarkingQueue *queue, Thread *thread, EntryStackFrame *entryFrame)
{
	while (entryFrame != NULL) {
		StackFrame *prev = entryFrame->exit;
		StackFrame *frame = stackFrameGetParent(prev, entryFrame);

		Value value = stackFrameGetSlot	(prev, 0);
		if (valueTypeOf(value, VALUE_POINTER)) {
			markObject(queue, thread, asObject(value));
		}

		while (frame != NULL) {
			NativeCode *code = stackFrameGetNativeCode(frame);
			size_t argsSize = code->argsSize + 1;
			for (ptrdiff_t i = 0; i < argsSize; i++) {
				Value value = stackFrameGetArg(frame, i);
				if (valueTypeOf(value, VALUE_POINTER)) {
					markObject(queue, thread, asObject(value));
				}
			}

			RawStackmap *stackmap = findStackmap(code, (ptrdiff_t) prev->parentIc);
			ASSERT(stackmap != NULL);
			size_t frameSize = (stackmap->size - sizeof(Value)) * 8;
			for (size_t i = 0; i < frameSize; i++) {
				if (stackmapIncludes(stackmap, i)) {
					Value value = stackFrameGetSlot(frame, i);
					if (valueTypeOf(value, VALUE_POINTER)) {
						markObject(queue, thread, asObject(value));
					}
				}
			}

			prev = frame;
			frame = stackFrameGetParent(frame, entryFrame);
		}
		entryFrame = entryFrame->prev;
	}
}


// Mark one fiber's stack-side roots: its entry block / process value, its saved
// stack frames, and its exception-handler chain.
static void iterateFiberStack(MarkingQueue *queue, Thread *thread, Fiber *fiber)
{
	if (valueTypeOf(fiber->entryBlock, VALUE_POINTER)) {
		markObject(queue, thread, asObject(fiber->entryBlock));
	}
	if (valueTypeOf(fiber->process, VALUE_POINTER)) {
		markObject(queue, thread, asObject(fiber->process));
	}
	iterateStackFrames(queue, thread, fiber->roots.stackFramesTail);
	iterateExceptionHandlerChain(queue, thread, fiber->roots.exceptionHandler);
}


// Mark ONE mutator's CURRENT live stack roots via its TLS (its parked fibers are marked
// once via the shared registry in iterateStack). Same `live` predicate as the scavenger's
// iterateMutatorCurrentRoots — see that comment for the four cases (bare mutator / pool
// worker running a fiber / the collector itself / an idle peer to skip).
static void iterateMutatorCurrentStack(MarkingQueue *queue, Thread *thread, Thread *m)
{
	Fiber *cur = (m->schedCurrent != NULL) ? *m->schedCurrent : NULL;
	_Bool live = (m->schedCurrent == NULL) || (cur != NULL) || (m == thread);
	if (!live) {
		return;
	}
	// A running fiber's STRUCT roots (entryBlock/process) are not in the TLS; the
	// registry walk skipped this fiber (state == RUNNING), so mark them here.
	if (cur != NULL) {
		if (valueTypeOf(cur->entryBlock, VALUE_POINTER)) {
			markObject(queue, thread, asObject(cur->entryBlock));
		}
		if (valueTypeOf(cur->process, VALUE_POINTER)) {
			markObject(queue, thread, asObject(cur->process));
		}
	}
	iterateStackFrames(queue, thread, m->stackFramesTail);
	if (m == thread) {
		iterateExceptionHandlerChain(queue, thread, CurrentExceptionHandler);
	} else if (m->schedExceptionHandler != NULL) {
		iterateExceptionHandlerChain(queue, thread, *m->schedExceptionHandler);
	}
}


static void iterateStack(MarkingQueue *queue, Thread *thread)
{
	Heap *heap = thread->heap;
	// M4 (shared registry): with ONE scheduler per heap, walk the shared registry ONCE,
	// skipping fibers RUNNING on a worker (their live stacks are marked via that worker's
	// TLS below). Marking is idempotent, but skipping keeps it symmetric with the scavenger.
	if (heap->sched != NULL) {
		size_t slots = schedulerFiberSlots();
		for (size_t i = 0; i < slots; i++) {
			Fiber *fiber = schedulerFiberAt(i);
			if (fiber != NULL && fiber->state != FIBER_RUNNING) {
				iterateFiberStack(queue, thread, fiber);
			}
		}
	}
	// Each mutator's currently-running fiber (or a bare mutator's C roots) via its TLS.
	for (Thread *m = heap->mutators; m != NULL; m = m->nextMutator) {
		iterateMutatorCurrentStack(queue, thread, m);
	}
}


static void iterateHandleScopes(MarkingQueue *queue, Thread *thread, HandleScope *scopes)
{
	HandleScopeIterator handleScopeIterator;
	initHandleScopeIterator(&handleScopeIterator, scopes);
	while (handleScopeIteratorHasNext(&handleScopeIterator)) {
		HandleScope *scope = handleScopeIteratorNext(&handleScopeIterator);
		for (ptrdiff_t i = 0; i < scope->size; i++) {
			markObject(queue, thread, scope->handles[i].raw);
		}
	}
}


// Mark one fiber's handle-side roots: its saved handle scopes and its context.
static void iterateFiberHandles(MarkingQueue *queue, Thread *thread, Fiber *fiber)
{
	iterateHandleScopes(queue, thread, fiber->roots.handleScopes);
	if (fiber->roots.context != 0) {
		markObject(queue, thread, asObject(fiber->roots.context));
	}
}


// Mark ONE mutator's CURRENT live handle-side roots via its TLS (its parked fibers are
// marked once via the shared registry in iterateHandles). Same `live` predicate as
// iterateMutatorCurrentStack.
static void iterateMutatorCurrentHandles(MarkingQueue *queue, Thread *thread, Thread *m)
{
	_Bool live = (m->schedCurrent == NULL)
		|| (*m->schedCurrent != NULL)
		|| (m == thread);
	if (!live) {
		return;
	}
	iterateHandleScopes(queue, thread, m->handleScopes);
	if (m->context != 0) {
		markObject(queue, thread, asObject(m->context));
	}
}


static void iterateHandles(MarkingQueue *queue, Thread *thread)
{
	Heap *heap = thread->heap;
	// Persistent handles are per-mutator: every OS thread sharing this heap owns
	// its own list (mirrors the scavenger's iteratePersistentHandles). Walk them
	// all so a full GC on any thread keeps every mutator's handled objects alive.
	for (Thread *m = heap->mutators; m != NULL; m = m->nextMutator) {
		HandlesIterator handlesIterator;
		initHandlesIterator(&handlesIterator, m->handles);
		while (handlesIteratorHasNext(&handlesIterator)) {
			markObject(queue, thread, handlesIteratorNext(&handlesIterator)->raw);
		}
	}

	// M4 (shared registry): walk the shared registry ONCE (skip RUNNING — via TLS below).
	if (heap->sched != NULL) {
		size_t slots = schedulerFiberSlots();
		for (size_t i = 0; i < slots; i++) {
			Fiber *fiber = schedulerFiberAt(i);
			if (fiber != NULL && fiber->state != FIBER_RUNNING) {
				iterateFiberHandles(queue, thread, fiber);
			}
		}
	}
	for (Thread *m = heap->mutators; m != NULL; m = m->nextMutator) {
		iterateMutatorCurrentHandles(queue, thread, m);
	}
}


static void iterateNativeCode(MarkingQueue *queue, Thread *thread)
{
	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &thread->heap->execSpace);
	NativeCode *code = (NativeCode *) pageSpaceIteratorNext(&iterator);

	while (code != NULL) {
		if ((code->tags & TAG_FREESPACE) == 0) {
			if (code->compiledCode != NULL) {
				markObject(queue, thread, (RawObject *) code->compiledCode);
			}
			if (code->stackmaps != NULL) {
				markObject(queue, thread, (RawObject *) code->stackmaps);
			}
			if (code->descriptors != NULL) {
				markObject(queue, thread, (RawObject *) code->descriptors);
			}
			if (code->typeFeedback != NULL) {
				markObject(queue, thread, (RawObject *) code->typeFeedback);
			}
			for (size_t i = 0; i < code->pointersOffsetsSize; i++) {
				uint16_t offset = ((uint16_t *) (code->insts + code->size))[i];
				Value value = *(Value *) (code->insts + offset);
				if (valueTypeOf(value, VALUE_POINTER)) {
					markObject(queue, thread, asObject(value));
				} else {
					markObject(queue, thread, (RawObject *) value);
				}
			}
		}
		code = (NativeCode *) pageSpaceIteratorNext(&iterator);
	}
}


static void iterateObject(MarkingQueue *queue, Thread *thread, RawObject *root)
{
	_Bool remember = isNewObject((RawObject *) root->class);
	markObject(queue, thread, (RawObject *) root->class);

	Value *vars = getRawObjectVars(root);
	size_t size = root->class->instanceShape.varsSize;
	if (root->class->instanceShape.isIndexed && !root->class->instanceShape.isBytes) {
		size += rawObjectSize(root);
	}

	for (size_t i = 0; i < size; i++) {
		if (valueTypeOf(vars[i], VALUE_POINTER)) {
			RawObject *object = asObject(vars[i]);
			remember = remember || isNewObject(object);
			markObject(queue, thread, object);
		}
	}

	root->tags = root->tags & ~TAG_REMEMBERED;
	if (remember && isOldObject(root)) {
		rememberedSetAdd(&thread->rememberedSet, root);
	}
}


static void markObject(MarkingQueue *queue, Thread *thread, RawObject *object)
{
	ASSERT(isOldObject(object) || (thread->heap->newSpace.fromSpace <= (uint8_t *) object && (uint8_t *) object <= (thread->heap->newSpace.fromSpace + thread->heap->newSpace.size)));
	if (object->tags & TAG_MARKED) {
		return;
	}
	// TODO: for now scavenge has to be called before mark&sweep to clear
	// marked tag of new objects
	object->tags |= TAG_MARKED;
	markingQueueAdd(queue, object);
	LastGCStats.marked++;
}


static void markingQueueAdd(MarkingQueue *queue, RawObject *object)
{
	if (queue->size == queue->index) {
		queue->size += QUEUE_INIT_SIZE;
		queue->objects = realloc(queue->objects, queue->size * sizeof(RawObject *));
	}
	queue->objects[queue->index++] = object;
}


static RawObject *markingQueuePop(MarkingQueue *queue)
{
	queue->index--;
	return queue->objects[queue->index];
}


static _Bool markingQueueIsEmpty(MarkingQueue *queue)
{
	return queue->index == 0;
}


void gcSweep(PageSpace *space)
{
	// Objects with finalizers found dead this sweep, run after the sweep. Grows on
	// demand — a fixed cap overflowed once full GCs became rare (grow-threshold
	// policy) and a sweep reclaims a large batch (e.g. many dead server sockets).
	RawObject **finalize = NULL;
	size_t finalizeSize = 0;
	size_t finalizeCap = 0;

	// Rebuild the freelist from scratch, fully coalesced. Resetting the bins first
	// lets a page that ends up ENTIRELY free be handed back to the OS (unmapped)
	// instead of lingering forever: otherwise, under a concurrent server, promoted-
	// then-dead objects keep the old-space page count — and thus every markAndSweep's
	// scan cost — growing without bound, so throughput drifts down over time and RSS
	// tracks the high-water mark. Consecutive dead objects / existing free chunks are
	// coalesced into one run; `createFreeSpace` writes the header only on flush, so
	// the mid-run objects are subsumed.
	memset(space->freeList.freeSpaces, 0, sizeof(space->freeList.freeSpaces));
	memset(space->freeList.freeMap, 0, sizeof(space->freeList.freeMap));

	uint8_t *runStart = NULL;
	size_t runSize = 0;
	// Flush a coalesced run to the freelist. For a large run (in a page we are
	// keeping) hand its interior's physical pages back to the OS with MADV_DONTNEED;
	// the 16-byte header (and rest of its page) stays for the freelist, and the
	// virtual mapping stays so a later split/alloc just re-faults zeroed pages.
	#define FLUSH_RUN() do { \
		if (runStart != NULL) { \
			freeListAddFreeSpace(&space->freeList, createFreeSpace(runStart, runSize)); \
			if (runSize >= 256 * 1024) { \
				uintptr_t lo = ((uintptr_t) runStart + sizeof(FreeSpace) + 4095) & ~(uintptr_t) 4095; \
				uintptr_t hi = ((uintptr_t) runStart + runSize) & ~(uintptr_t) 4095; \
				if (hi > lo) madvise((void *) lo, hi - lo, MADV_DONTNEED); \
			} \
			runStart = NULL; runSize = 0; \
		} \
	} while (0)

	HeapPage *prev = NULL;
	HeapPage *page = space->pages;
	while (page != NULL) {
		HeapPage *nextPage = page->next;
		uint8_t *p = (uint8_t *) align((uintptr_t) page->body, HEAP_OBJECT_ALIGN);
		uint8_t *pageEnd = page->body + page->bodySize;
		_Bool pageHasLive = 0;
		runStart = NULL;
		runSize = 0;

		while (p < pageEnd) {
			RawObject *object = (RawObject *) p;
			LastGCStats.total++;
			size_t objSize;
			if ((object->tags & TAG_FREESPACE) != 0) {
				objSize = align(((FreeSpace *) object)->size, HEAP_OBJECT_ALIGN);
			} else {
				objSize = align(computeRawObjectSize(object), HEAP_OBJECT_ALIGN);
				if ((object->tags & TAG_MARKED) != 0) {
					FLUSH_RUN();
					object->tags = object->tags ^ TAG_MARKED;
					pageHasLive = 1;
					p += objSize;
					continue;
				}
				if ((object->tags & TAG_FINALIZED) == 0 && hasFinalizer(object)) {
					FLUSH_RUN();
					if (finalizeSize == finalizeCap) {
						finalizeCap = finalizeCap ? finalizeCap * 2 : 256;
						finalize = realloc(finalize, finalizeCap * sizeof(RawObject *));
					}
					finalize[finalizeSize++] = object;
					object->tags = (object->tags ^ TAG_MARKED) | TAG_FINALIZED;
					pageHasLive = 1;
					p += objSize;
					continue;
				}
				LastGCStats.freed++;
				LastGCStats.sweeped++;
			}
			// free region (existing free chunk, or an unmarked dead object): coalesce
			if (runStart != NULL && (uint8_t *) object == runStart + runSize) {
				runSize += objSize;
			} else {
				FLUSH_RUN();
				runStart = (uint8_t *) object;
				runSize = objSize;
			}
			p += objSize;
		}

		// Keep the head page (it anchors the list and holds always-live kernel
		// objects); reclaim any other page that turned out entirely free.
		if (pageHasLive || prev == NULL) {
			FLUSH_RUN();
			prev = page;
		} else {
			runStart = NULL;
			runSize = 0; // discard the page-spanning free run; the page is going away
			prev->next = nextPage;
			if (space->pagesTail == page) {
				space->pagesTail = prev;
			}
			pageSpaceIndexRemove(space, page);
			unmapHeapPage(page);
		}
		page = nextPage;
	}
	#undef FLUSH_RUN

	for (size_t i = 0; i < finalizeSize; i++) {
		HandleScope scope;
		openHandleScope(&scope);
		EntryArgs args = { .size = 0 };
		entryArgsAddObject(&args, scopeHandle(finalize[i]));
		sendMessage(Handles.finalizeSymbol, &args);
		closeHandleScope(&scope, NULL);
	}
	free(finalize);
}


static _Bool hasFinalizer(RawObject *object)
{
	HandleScope scope;
	openHandleScope(&scope);
	// TODO: maybe add empty implementation in Object and check if #finalize was overwritten in subclass
	_Bool hasFinalizer = lookupSelector(scopeHandle(object->class), Handles.finalizeSymbol) != NULL;
	closeHandleScope(&scope, NULL);
	return hasFinalizer;
}


void resetGcStats(void)
{
	LastGCStats.total = 0;
	LastGCStats.marked = 0;
	LastGCStats.sweeped = 0;
	LastGCStats.freed = 0;
	LastGCStats.extended = 0;
}


void printGcStats(void)
{
	printf(
		"GC total: %zu"
		" marked: %zu"
		" sweeped: %zu"
		" freed: %zu"
		" extended: %zu"
		" time: %" PRIu64
		" count: %zu\n",
		LastGCStats.total,
		LastGCStats.marked,
		LastGCStats.sweeped,
		LastGCStats.freed,
		LastGCStats.extended,
		LastGCStats.totalTime,
		LastGCStats.count
	);
}
