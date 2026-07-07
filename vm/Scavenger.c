#include "Scavenger.h"
#include "Heap.h"
#include "StackFrame.h"
#include "CodeDescriptors.h"
#include "Thread.h"
#include "Exception.h"
#include "Scheduler.h"
#include "Fiber.h"
#include "GarbageCollector.h"
#include "Os.h"
#include <string.h>

#define SCAVENGER_ALIGN 8

static void iterateFiberRoots(Scavenger *scavenger, Fiber *fiber);
static void iteratePersistentHandles(Scavenger *scavenger);
static void iterateThreadRoots(Scavenger *scavenger);
static void iterateExceptionHandlerSlot(Scavenger *scavenger, Value *slot);
static void iterateHandleScopes(Scavenger *scavenger, HandleScope *scopes);
static void iterateRememberedSet(Scavenger *scavenger);
static void iterateNativeCode(Scavenger *scavenger);
static RawObject *processPointer(Scavenger *scavenger, RawObject **p);
static RawObject *processTaggedPointer(Scavenger *scavenger, Value *p);
static void forwardObject(Scavenger *scavenger, RawObject *object);
static void iterateObject(Scavenger *scavenger, RawObject *root);


void initScavenger(Scavenger *scavenger, Heap *heap, size_t size)
{
	scavenger->heap = heap;
	scavenger->page = mapHeapPage(size, 0);
	size_t semiSpaceSize = scavenger->page->bodySize / 2;
	uint8_t *start = scavenger->page->body;

	scavenger->size = semiSpaceSize;

	scavenger->fromSpace = start;
	scavenger->toSpace = start + semiSpaceSize;

	scavenger->top = (uint8_t *) ((uintptr_t) start | NEW_SPACE_TAG);
	scavenger->end = start + semiSpaceSize;
	scavenger->survivorEnd = scavenger->top;
}


void freeScavenger(Scavenger *scavenger)
{
	unmapHeapPage(scavenger->page);
}


uint8_t *scavengerTryAllocate(Scavenger *scavenger, size_t size)
{
	ASSERT(size % HEAP_OBJECT_ALIGN == 0);
	size_t available = scavenger->end - scavenger->top;
	if (size > available) {
		return NULL;
	}

	uint8_t *result = scavenger->top;
	ASSERT(((uintptr_t) result & SPACE_TAG) == NEW_SPACE_TAG);
	ASSERT(scavenger->fromSpace <= result && result <= (scavenger->fromSpace + scavenger->size));
	scavenger->top += size;
	return result;
}


void scavengerScavenge(Scavenger *scavenger)
{
	int64_t scavengeStart = osCurrentMicroTime();
	scavenger->hasPromotionFailure = 0;
	scavenger->top = (uint8_t *) ((uintptr_t) scavenger->toSpace | NEW_SPACE_TAG);
	scavenger->end = scavenger->toSpace + scavenger->size;

	uint8_t *fromSpace = scavenger->toSpace;
	uint8_t *toSpace = scavenger->fromSpace;
	scavenger->fromSpace = fromSpace;
	scavenger->toSpace = toSpace;

	schedulerSyncCurrentRoots();
	iterateRememberedSet(scavenger);       // shared: covers every old→young edge
	iteratePersistentHandles(scavenger);   // shared across all fibers
	if (schedulerActive()) {
		// Walk ONLY dirty fibers; clean any (non-current) whose direct roots are
		// all old — their reachable young objects are already covered by the
		// remembered set. This is what turns O(fibers × scavenges) into
		// O(dirty × scavenges).
		Fiber *current = schedulerCurrentFiber();
		Fiber *fiber = schedulerDirtyHead();
		while (fiber != NULL) {
			Fiber *next = fiber->dirtyNext; // capture before a possible unlink
			scavenger->fiberHasYoungRoot = 0;
			iterateFiberRoots(scavenger, fiber);
			if (fiber != current && !scavenger->fiberHasYoungRoot) {
				schedulerMarkFiberClean(fiber);
			}
			fiber = next;
		}
	} else {
		iterateThreadRoots(scavenger); // bootstrap: no scheduler yet
	}
	iterateNativeCode(scavenger);
	schedulerRestoreCurrentRoots();
	scavenger->survivorEnd = scavenger->top;
	memset(scavenger->toSpace, scavenger->size, 0);

	LastGCStats.scavengeCount++;
	LastGCStats.scavengeTimeUs += osCurrentMicroTime() - scavengeStart;

#if VERIFY_HEAP_AFTER_GC
	verifyHeap(scavenger->heap);
#endif
}


_Bool scavengerIncludes(Scavenger *scavenger, uint8_t *addr)
{
	uint8_t *start = (uint8_t *) ((uintptr_t) scavenger->fromSpace | NEW_SPACE_TAG);
	return start <= addr && addr < scavenger->top;
}


// Process one stackmap-live frame slot.
//
// A stackmap slot can legitimately still hold the prologue's nil-init value when
// the running code has not assigned it on the path actually taken — most notably
// the result temp of an inlined `ifTrue:ifFalse:`, which the register allocator
// (linear-scan, control-flow-unaware) marks live from its first arm store even at
// call sites in the *other* arm that precede any store. `nil` (and true/false)
// are ordinary movable young objects, so once nil-init writes `nil` into such a
// slot and a later scavenge moves `nil` without walking this (then-unmarked)
// frame, the slot is left pointing at nil's stale location.
//
// A young-tagged pointer that is NOT inside the space currently being evacuated
// is exactly that stale nil-init value (its true logical value is nil). Restore
// the current `nil` and forward it normally, honouring the nil-init contract.
static _Bool ptrInHeap(Scavenger *scavenger, uint8_t *addr)
{
	return pageSpaceIncludes(&scavenger->heap->oldSpace, addr)
		|| scavengerIncludes(scavenger, addr)
		|| (scavenger->toSpace <= addr && addr <= scavenger->toSpace + scavenger->size);
}

// Does `object` (a young pointer that lands in the source space) actually look
// like a live object, or is it a stale value left in a dead / not-yet-assigned
// inlined-control-flow slot? A live object's class is a real heap pointer and its
// computed size is bounded by a semispace; garbage fails one of these (the crash
// symptom is a wild size fed to the promotion allocator). The class must be
// validated as mapped BEFORE dereferencing it for the size (an old-space page
// walk) — trusting the space tag alone segfaults on an unmapped garbage class.
static _Bool plausibleObject(Scavenger *scavenger, RawObject *object)
{
	uint8_t *class = (uint8_t *) ((uintptr_t) object->class & ~(uintptr_t) SPACE_TAG);
	if (!ptrInHeap(scavenger, class)) {
		return 0;
	}
	size_t size = computeRawObjectSize(object);
	return size > 0 && size <= (size_t) scavenger->size;
}

// Record whether a DIRECT fiber root slot, AFTER processing, still holds a young
// pointer. A fiber with no young direct root can be skipped by future scavenges
// (its reachable young objects are covered by the remembered set). Must be called
// on the post-processing value: stale nil-init garbage has been rewritten to old
// `nil`, and a genuine young survivor stays young until it promotes (≤2 scavenges).
static inline void noteYoungRoot(Scavenger *scavenger, Value v)
{
	if (valueTypeOf(v, VALUE_POINTER) && isNewObject(asObject(v))) {
		scavenger->fiberHasYoungRoot = 1;
	}
}

static void scavengeStackSlot(Scavenger *scavenger, Value *value)
{
	RawObject *object = asObject(*value);
	if (((uintptr_t) object & SPACE_TAG) != OLD_SPACE_TAG
		&& !(scavenger->toSpace <= (uint8_t *) object && (uint8_t *) object <= scavenger->toSpace + scavenger->size)) {
		*value = tagPtr(Handles.nil->raw);
		processTaggedPointer(scavenger, value);
		return;
	}

	// A young pointer that lands INSIDE the source space can still be stale:
	// semispaces swap each scavenge, so a nil-init / prior-loop-iteration value
	// left in an inlined-control-flow result temp (marked live by the CF-unaware
	// linear-scan allocator at a call site that precedes its store on the taken
	// path) can fall within the current source space yet point at no live object
	// (seen under load in HttpServer>>serve:'s whileTrue: block, slot 4). Tell a
	// live object from that garbage by validating its header (real class pointer +
	// a size bounded by a semispace); if implausible, the slot was never validly
	// assigned on this path -> its logical value is nil.
	if (((uintptr_t) object & SPACE_TAG) != OLD_SPACE_TAG
		&& (object->tags & TAG_FORWARDED) == 0
		&& !plausibleObject(scavenger, object)) {
		*value = tagPtr(Handles.nil->raw);
	}
	processTaggedPointer(scavenger, value);
	noteYoungRoot(scavenger, *value);
}


static void iterateStackFrames(Scavenger *scavenger, EntryStackFrame *entryFrame)
{
	while (entryFrame != NULL) {
		StackFrame *prev = entryFrame->exit;
		StackFrame *frame = stackFrameGetParent(prev, entryFrame);
		Value *slot0 = stackFrameGetSlotPtr(prev, 0);
		processTaggedPointer(scavenger, slot0);
		noteYoungRoot(scavenger, *slot0);

		while (frame != NULL) {
			NativeCode *code = stackFrameGetNativeCode(frame);
			ASSERT(code->insts <= prev->parentIc && prev->parentIc <= (code->insts + code->size));

			size_t argsSize = code->argsSize + 1;
			for (ptrdiff_t i = 0; i < argsSize; i++) {
				Value *value = stackFrameGetArgPtr(frame, i);
				if (valueTypeOf(*value, VALUE_POINTER)) {
					scavengeStackSlot(scavenger, value);
				}
			}

			RawStackmap *stackmap = findStackmap(code, (ptrdiff_t) prev->parentIc);
			ASSERT(stackmap != NULL);
			size_t frameSize = (stackmap->size - sizeof(Value)) * 8;
			for (size_t i = 0; i < frameSize; i++) {
				//ASSERT(i != 0 || stackmapIncludes(stackmap, i));
				if (stackmapIncludes(stackmap, i)) {
					Value *value = stackFrameGetSlotPtr(frame, i);
					if (valueTypeOf(*value, VALUE_POINTER)) {
						//ASSERT(pageSpaceIncludes(&_Heap.space, (uint8_t *) asObject(value)));
						scavengeStackSlot(scavenger, value);
					}
				}
			}

			prev = frame;
			frame = stackFrameGetParent(frame, entryFrame);
		}
		entryFrame = entryFrame->prev;
	}
}


// Walk one fiber's six DIRECT roots in the required per-fiber order (entryBlock/
// process, then stack → exception handler → handle scopes → context — the
// exception-handler walk reads the fiber's already-forwarded stack). Sets
// scavenger->fiberHasYoungRoot if any direct slot ends up young after processing.
static void iterateFiberRoots(Scavenger *scavenger, Fiber *fiber)
{
	if (valueTypeOf(fiber->entryBlock, VALUE_POINTER)) {
		processTaggedPointer(scavenger, &fiber->entryBlock);
		noteYoungRoot(scavenger, fiber->entryBlock);
	}
	if (valueTypeOf(fiber->process, VALUE_POINTER)) {
		processTaggedPointer(scavenger, &fiber->process);
		noteYoungRoot(scavenger, fiber->process);
	}
	iterateStackFrames(scavenger, fiber->roots.stackFramesTail);
	iterateExceptionHandlerSlot(scavenger, &fiber->roots.exceptionHandler);
	iterateHandleScopes(scavenger, fiber->roots.handleScopes);
	if (fiber->roots.context != 0) {
		processTaggedPointer(scavenger, &fiber->roots.context);
		noteYoungRoot(scavenger, fiber->roots.context);
	}
}


// Non-scheduler (bootstrap) path: the single thread's roots, walked directly.
static void iterateThreadRoots(Scavenger *scavenger)
{
	Thread *thread = scavenger->heap->thread;
	iterateStackFrames(scavenger, thread->stackFramesTail);
	iterateExceptionHandlerSlot(scavenger, &CurrentExceptionHandler);
	iterateHandleScopes(scavenger, thread->handleScopes);
	if (thread->context != 0) {
		processTaggedPointer(scavenger, &thread->context);
	}
}


static void iterateExceptionHandlerSlot(Scavenger *scavenger, Value *slot)
{
	Value handlerValue = *slot;

	while (handlerValue != 0) {
		RawExceptionHandler *handler = (RawExceptionHandler *) asObject(handlerValue);
		RawContext *context = (RawContext *) asObject(handler->context);
		if (contextHasValidFrame(context)) {
			*slot = handlerValue;
			break;
		}
		handlerValue = handler->parent;
	}
	if (handlerValue == 0) {
		*slot = 0;
	}

	if (*slot != 0) {
		processTaggedPointer(scavenger, slot);
		noteYoungRoot(scavenger, *slot);
	}
}


static void iterateHandleScopes(Scavenger *scavenger, HandleScope *scopes)
{
	HandleScopeIterator handleScopeIterator;
	initHandleScopeIterator(&handleScopeIterator, scopes);
	while (handleScopeIteratorHasNext(&handleScopeIterator)) {
		HandleScope *scope = handleScopeIteratorNext(&handleScopeIterator);
		for (ptrdiff_t i = 0; i < scope->size; i++) {
			RawObject *obj = processPointer(scavenger, &scope->handles[i].raw);
			if (isNewObject(obj)) {
				scavenger->fiberHasYoungRoot = 1;
			}
		}
	}
}


// The persistent handle list is SHARED across all fibers — walked once per
// scavenge regardless of the dirty-fiber optimization. (Per-fiber handle scopes
// and context are walked in iterateFiberRoots.)
static void iteratePersistentHandles(Scavenger *scavenger)
{
	Thread *thread = scavenger->heap->thread;
	HandlesIterator handlesIterator;
	initHandlesIterator(&handlesIterator, thread->handles);
	while (handlesIteratorHasNext(&handlesIterator)) {
		processPointer(scavenger, &handlesIteratorNext(&handlesIterator)->raw);
	}
}


static void iterateRememberedSet(Scavenger *scavenger)
{
	// Rebuild the remembered set from scratch each scavenge: detach the current
	// entries, then re-scan each remembered old object. iterateObject's tail
	// re-adds it — into the FRESH set — iff it STILL points into the new young
	// space after its young referents are forwarded; entries whose referents were
	// all promoted (or are dead) are dropped. Clearing TAG_REMEMBERED first is
	// what lets iterateObject re-add it.
	//
	// Previously the set was only pruned at a full GC (rememberedSetReset), and
	// full GCs get exponentially rarer as old space grows — so under a sustained
	// actor/message workload it grew without bound and every scavenge re-scanned
	// an ever-larger set (O(scavenges × set)), collapsing req/s and, because a
	// dead old entry keeps resurrecting its young referents, exploding old space.
	RememberedSet *rememberedSet = &scavenger->heap->rememberedSet;
	RememberedSet detached;
	detached.blocks = rememberedSet->blocks;
	rememberedSet->blocks = createRememberedSetBlock(NULL);

	RememberedSetIterator iterator;
	initRememberedSetIterator(&iterator, &detached);
	while (rememberedSetIteratorHasNext(&iterator)) {
		RawObject *root = rememberedSetIteratorNext(&iterator);
		root->tags &= ~TAG_REMEMBERED;
		iterateObject(scavenger, root);
	}

	rememberedSetFreeBlocks(detached.blocks);
}


static void iterateNativeCode(Scavenger *scavenger)
{
	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &scavenger->heap->execSpace);
	NativeCode *code = (NativeCode *) pageSpaceIteratorNext(&iterator);

	while (code != NULL) {
		if ((code->tags & TAG_FREESPACE) == 0) {
			if (code->compiledCode != NULL) {
				processPointer(scavenger, (RawObject **) &code->compiledCode);
			}
			if (code->stackmaps != NULL) {
				processPointer(scavenger, (RawObject **) &code->stackmaps);
			}
			if (code->descriptors != NULL) {
				processPointer(scavenger, (RawObject **) &code->descriptors);
			}
			if (code->typeFeedback != NULL) {
				processPointer(scavenger, (RawObject **) &code->typeFeedback);
			}
			for (size_t i = 0; i < code->pointersOffsetsSize; i++) {
				uint16_t offset = ((uint16_t *) (code->insts + code->size))[i];
				Value *ptr = (Value *) (code->insts + offset);
				if (valueTypeOf(*ptr, VALUE_POINTER)) {
					processTaggedPointer(scavenger, ptr);
				} else {
					processPointer(scavenger, (RawObject **) ptr);
				}
			}
		}
		code = (NativeCode *) pageSpaceIteratorNext(&iterator);
	}
}


static RawObject *processPointer(Scavenger *scavenger, RawObject **p)
{
	RawObject *object = *p;
	if (((uintptr_t) object & SPACE_TAG) == OLD_SPACE_TAG) {
		return object;
	}
	if (object->tags & TAG_FORWARDED) {
		ASSERT(isOldObject((RawObject *) object->class) || scavenger->fromSpace <= (uint8_t *) object->class && (uint8_t *) object->class <= scavenger->top);
		ASSERT((object->class->tags & TAG_FORWARDED) == 0);
		*p = (RawObject *) object->class;
		return (RawObject *) object->class;
	}

	forwardObject(scavenger, object);
	ASSERT((object->class->tags & TAG_FORWARDED) == 0);
	object = (RawObject *) object->class;
	*p = object;
	iterateObject(scavenger, object);
	return object;
}


static RawObject *processTaggedPointer(Scavenger *scavenger, Value *p)
{
	RawObject *object = asObject(*p);
	if (((uintptr_t) object & SPACE_TAG) == OLD_SPACE_TAG) {
		return object;
	}
	if (object->tags & TAG_FORWARDED) {
		ASSERT(isOldObject((RawObject *) object->class) || scavenger->fromSpace <= (uint8_t *) object->class && (uint8_t *) object->class < scavenger->top);
		*p = tagPtr(object->class);
		return (RawObject *) object->class;
	}

	forwardObject(scavenger, object);
	object = (RawObject *) object->class;
	*p = tagPtr(object);
	iterateObject(scavenger, object);
	return object;
}


static void forwardObject(Scavenger *scavenger, RawObject *object)
{
	ASSERT((object->tags & TAG_FORWARDED) == 0);
	ASSERT(scavenger->toSpace <= (uint8_t *) object && (uint8_t *) object <= (scavenger->toSpace + scavenger->size));

	size_t size = align(computeRawObjectSize(object), HEAP_OBJECT_ALIGN);
	RawObject *newObject;

	object->tags &= ~TAG_MARKED;

	if ((uint8_t *) object < scavenger->survivorEnd) {
		newObject = (RawObject *) tryAllocateOld(scavenger->heap, size, scavenger->hasPromotionFailure);
		if (newObject == NULL) {
			scavenger->hasPromotionFailure = 1;
			newObject = (RawObject *) tryAllocateOld(scavenger->heap, size, 1);
		}
	} else {
		newObject = (RawObject *) scavengerTryAllocate(scavenger, size);
		ASSERT(scavenger->fromSpace <= (uint8_t *) newObject && (uint8_t *) newObject <= (scavenger->fromSpace + scavenger->size));
	}

	ASSERT(newObject != NULL);
	memcpy(newObject, object, size);
	object->tags |= TAG_FORWARDED;
	object->class = (RawClass *) newObject;
}


static void iterateObject(Scavenger *scavenger, RawObject *root)
{
	RawObject *object = processPointer(scavenger, (RawObject **) &root->class);
	_Bool remember = isNewObject(object);

	Value *vars = getRawObjectVars(root);
	size_t size = root->class->instanceShape.varsSize;
	if (root->class->instanceShape.isIndexed && !root->class->instanceShape.isBytes) {
		size += rawObjectSize(root);
	}

	for (size_t i = 0; i < size; i++) {
		if (valueTypeOf(vars[i], VALUE_POINTER)) {
			RawObject *object = processTaggedPointer(scavenger, &vars[i]);
			remember = remember || isNewObject(object);
		}
	}

	if (remember && isOldObject(root) && (root->tags & TAG_REMEMBERED) == 0) {
		rememberedSetAdd(&scavenger->heap->rememberedSet, root);
	}
}
