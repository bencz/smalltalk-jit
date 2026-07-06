#include "Scavenger.h"
#include "Heap.h"
#include "StackFrame.h"
#include "CodeDescriptors.h"
#include "Thread.h"
#include "Exception.h"
#include "Scheduler.h"
#include "Fiber.h"
#include <string.h>

#define SCAVENGER_ALIGN 8

static void iterateStack(Scavenger *scavenger);
static void iterateExceptionHandlers(Scavenger *scavenger);
static void iterateHandles(Scavenger *scavenger);
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
	scavenger->hasPromotionFailure = 0;
	scavenger->top = (uint8_t *) ((uintptr_t) scavenger->toSpace | NEW_SPACE_TAG);
	scavenger->end = scavenger->toSpace + scavenger->size;

	uint8_t *fromSpace = scavenger->toSpace;
	uint8_t *toSpace = scavenger->fromSpace;
	scavenger->fromSpace = fromSpace;
	scavenger->toSpace = toSpace;

	schedulerSyncCurrentRoots();
	iterateRememberedSet(scavenger);
	iterateStack(scavenger);
	iterateExceptionHandlers(scavenger);
	iterateHandles(scavenger);
	iterateNativeCode(scavenger);
	schedulerRestoreCurrentRoots();
	scavenger->survivorEnd = scavenger->top;
	memset(scavenger->toSpace, scavenger->size, 0);

#if VERIFY_HEAP_AFTER_GC
	verifyHeap();
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
}


static void iterateStackFrames(Scavenger *scavenger, EntryStackFrame *entryFrame)
{
	while (entryFrame != NULL) {
		StackFrame *prev = entryFrame->exit;
		StackFrame *frame = stackFrameGetParent(prev, entryFrame);
		processTaggedPointer(scavenger, stackFrameGetSlotPtr(prev, 0));

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


static void iterateStack(Scavenger *scavenger)
{
	if (!schedulerActive()) {
		iterateStackFrames(scavenger, scavenger->heap->thread->stackFramesTail);
		return;
	}
	size_t slots = schedulerFiberSlots();
	for (size_t i = 0; i < slots; i++) {
		Fiber *fiber = schedulerFiberAt(i);
		if (fiber == NULL) {
			continue;
		}
		if (valueTypeOf(fiber->entryBlock, VALUE_POINTER)) {
			processTaggedPointer(scavenger, &fiber->entryBlock);
		}
		if (valueTypeOf(fiber->process, VALUE_POINTER)) {
			processTaggedPointer(scavenger, &fiber->process);
		}
		iterateStackFrames(scavenger, fiber->roots.stackFramesTail);
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
	}
}


static void iterateExceptionHandlers(Scavenger *scavenger)
{
	if (!schedulerActive()) {
		iterateExceptionHandlerSlot(scavenger, &CurrentExceptionHandler);
		return;
	}
	size_t slots = schedulerFiberSlots();
	for (size_t i = 0; i < slots; i++) {
		Fiber *fiber = schedulerFiberAt(i);
		if (fiber != NULL) {
			iterateExceptionHandlerSlot(scavenger, &fiber->roots.exceptionHandler);
		}
	}
}


static void iterateHandleScopes(Scavenger *scavenger, HandleScope *scopes)
{
	HandleScopeIterator handleScopeIterator;
	initHandleScopeIterator(&handleScopeIterator, scopes);
	while (handleScopeIteratorHasNext(&handleScopeIterator)) {
		HandleScope *scope = handleScopeIteratorNext(&handleScopeIterator);
		for (ptrdiff_t i = 0; i < scope->size; i++) {
			processPointer(scavenger, &scope->handles[i].raw);
		}
	}
}


static void iterateHandles(Scavenger *scavenger)
{
	Thread *thread = scavenger->heap->thread;

	// persistent handle list is shared across all fibers
	HandlesIterator handlesIterator;
	initHandlesIterator(&handlesIterator, thread->handles);
	while (handlesIteratorHasNext(&handlesIterator)) {
		processPointer(scavenger, &handlesIteratorNext(&handlesIterator)->raw);
	}

	if (!schedulerActive()) {
		iterateHandleScopes(scavenger, thread->handleScopes);
		if (thread->context != 0) {
			processTaggedPointer(scavenger, &thread->context);
		}
		return;
	}

	size_t slots = schedulerFiberSlots();
	for (size_t i = 0; i < slots; i++) {
		Fiber *fiber = schedulerFiberAt(i);
		if (fiber == NULL) {
			continue;
		}
		iterateHandleScopes(scavenger, fiber->roots.handleScopes);
		if (fiber->roots.context != 0) {
			processTaggedPointer(scavenger, &fiber->roots.context);
		}
	}
}


static void iterateRememberedSet(Scavenger *scavenger)
{
	RememberedSetIterator iterator;
	initRememberedSetIterator(&iterator, &scavenger->heap->rememberedSet);
	while (rememberedSetIteratorHasNext(&iterator)) {
		iterateObject(scavenger, rememberedSetIteratorNext(&iterator));
	}
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
