// Identity, and the reflection that reads an object's own header.
//
// Nothing here allocates: every answer is an immediate, or a class the caller
// already reaches through the class table.
//
// A NOTE ON WHAT IS NOT HERE. This file used to carry primNotIdentical as well,
// and the split is what made it visible: runtime/Primitives.def never named it,
// so it was unreachable from the table and had been since it was written. The
// kernel writes `~~` in Smalltalk (`^self == anObject == false`, Object.st:125)
// and declares no pragma for it, so there is no primitive to be the far side of.
// It was removed rather than exported.

#include "runtime/primitives/Shared.h"
#include <string.h>


Value primIdentical(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	// Identity is Value equality, and that is exactly right for immediates too:
	// two equal SmallIntegers are the same object because the value IS the
	// object, while two BoxedFloat64s holding the same double are not.
	return booleanResult(primitiveReceiver(args) == primitiveArgument(args, 0));
}


Value primClass(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// classOf is the one place that knows immediates have classes too, so this
	// works for a SmallInteger as well as for a heap object.
	return tagPtr(classOf(primitiveReceiver(args)));
}


Value primIdentityHash(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	switch (receiver & 3) {
	case VALUE_INT:
		return receiver; // a SmallInteger hashes as itself
	case VALUE_CHAR:
		return tagInt((intptr_t) (receiver >> 2));
	case VALUE_POINTER:
		return tagInt((intptr_t) rawObjectHash(asObject(receiver)));
	default:
		// A Float's hash has to agree with numeric equality across the whole
		// tower, which is a Smalltalk-side decision rather than a header read.
		return PRIMITIVE_FAILED;
	}
}


// The packed shape word of a class, which Smalltalk cannot read as a field:
// jit-v2 keeps it in the class's RAW TRAILER so the collector never walks it
// (ADR 0005), and that is exactly the kind of thing a primitive is for.
Value primInstanceShape(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Class *class = receiverAsClass(primitiveReceiver(args));
	if (class == NULL) {
		return PRIMITIVE_FAILED;
	}
	// PACKED FIELD BY FIELD, and not by memcpy of the struct.
	//
	// InstanceShape has PADDING between pointerWords and fixedSlots, and the C
	// standard leaves padding bytes indeterminate. A memcpy therefore hands
	// Smalltalk a word with garbage bits in it, and the number a class answers
	// for its own shape could differ between two builds, or two runs, for
	// reasons no reader could see.
	//
	// The layout below is the CONTRACT with Behavior in packages/Core, which
	// decodes it. It is stated here and repeated there, and nowhere else:
	//
	//   bits  0..7   format (ObjectFormat)
	//   bits  8..15  rawWords
	//   bits 16..23  pointerWords
	//   bits 24..39  fixedSlots
	InstanceShape shape = class->raw->instanceShape;
	uint64_t packed = (uint64_t) shape.format
		| ((uint64_t) shape.rawWords << 8)
		| ((uint64_t) shape.pointerWords << 16)
		| ((uint64_t) shape.fixedSlots << 24);
	return tagInt((intptr_t) packed);
}


// ---------------------------------------------------------------------------
// become:
// ---------------------------------------------------------------------------
//
// `a become: b` makes the receiver BE the argument: every reference that named
// a names b afterwards, and `a identityHash = b identityHash` holds. That is
// one-way, not a swap, and it is what tests/ObjectTest.st pins in both of its
// branches -- a two-way exchange would leave the two hashes different, which is
// exactly what the checks say must not happen.
//
// ADR 0003 lists it as R8, "preserve what already works and is not negotiable",
// and it is a constraint on the COLLECTOR rather than a feature of this file:
// it is only implementable because the old space does not move (ADR 0005) and
// because a free chunk is itself a FORMAT_FREE object, so both spaces can be
// walked in a straight line.
//
// TWO CASES, and the size decides:
//
//   SAME SIZE   copy the argument OVER the receiver, header included. The
//               object at the receiver's address then IS the argument in every
//               observable way, hash included, and not one reference had to be
//               found. This is the cheap and overwhelmingly common case.
//   DIFFERENT   there is nowhere to copy to, so every reference is rewritten:
//               the old space, the young space, and every root.
//
// WHAT MAKES THE WALK LEGAL is that nothing else is running. The scheduler is
// cooperative and single-threaded (concurrency/Scheduler.h), so no peer can
// allocate underneath it; and heapFillAllTlabTails caps every mutator's TLAB
// with a free chunk first, because the tail of a TLAB is UNINITIALISED and a
// linear walk would read a header out of it. v1 needed a stop-the-world
// handshake here for the same reason; this needs the filler and nothing else.

#include "memory/Collector.h"
#include "memory/ObjectWalk.h"
#include "memory/PageSpace.h"
#include "memory/Nursery.h"

typedef struct {
	RawObject *from;
	RawObject *to;
} BecomeRewrite;


// Rewrite one slot, THROUGH THE BARRIER. The new reference may be young and the
// object holding it old, which is precisely the case the remembered set exists
// for; a bare store here would lose it and the next young collection would free
// an object that is still referenced.
static void becomeSlot(RawObject *owner, Value *slot, BecomeRewrite *rewrite)
{
	if (!valueTypeOf(*slot, VALUE_POINTER) || asObject(*slot) != rewrite->from) {
		return;
	}
	if (owner != NULL) {
		rawObjectStorePtr(owner, slot, rewrite->to);
	} else {
		*slot = tagPtr(rewrite->to);
	}
}


// The root visitor. Roots live outside the heap -- the class table, the
// well-known handles, C handle scopes, compiled frames, unwind records, every
// parked fiber -- so there is no owner to remember and a plain store is right.
static void becomeRoot(void *context, Value *slot)
{
	becomeSlot(NULL, slot, (BecomeRewrite *) context);
}


static void becomeInObject(RawObject *object, BecomeRewrite *rewrite,
	ClassTable *classes)
{
	// A free chunk and a forwarded object have no slots to rewrite, and reading
	// their bodies as pointers is exactly the mistake the FORMAT_FREE design
	// exists to prevent.
	uint8_t format = rawObjectFormat(object);
	if (format == FORMAT_FREE || format == FORMAT_FORWARDED) {
		return;
	}
	size_t count = 0;
	Value *slots = objectPointerSlots(classes, object, &count);
	for (size_t i = 0; i < count; i++) {
		becomeSlot(object, &slots[i], rewrite);
	}
}


// Object>>become: otherObject
Value primBecome(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value other = primitiveArgument(args, 0);
	// IMMEDIATES CANNOT BECOME ANYTHING. A SmallInteger is not at an address, so
	// there is no reference to rewrite and no body to copy; failing is the only
	// honest answer and the kernel's fallback raises.
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(other, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *from = asObject(receiver);
	RawObject *to = asObject(other);
	if (from == to) {
		return other; // already itself; nothing to do and nothing to walk
	}

	Heap *heap = CurrentThread.heap;
	ClassTable *classes = &heap->classes;
	size_t fromSize = objectSizeInBytes(classes, from);
	size_t toSize = objectSizeInBytes(classes, to);

	if (fromSize == toSize) {
		memcpy(from, to, fromSize);
		// THE BARRIER IS RE-ESTABLISHED, and forgetting it is the silent half of
		// this case: the copy brought the argument's header along, remembered
		// bit included, so an OLD receiver that now holds YOUNG references would
		// no longer be in the remembered set. Storing every pointer slot onto
		// itself re-runs the barrier for exactly the slots that need it.
		size_t count = 0;
		Value *slots = objectPointerSlots(classes, from, &count);
		for (size_t i = 0; i < count; i++) {
			if (valueTypeOf(slots[i], VALUE_POINTER)) {
				rawObjectStorePtr(from, &slots[i], asObject(slots[i]));
			}
		}
		return other;
	}

	// The sizes differ, so every reference has to be found. Nothing may allocate
	// from here on: the walk reads headers, and an allocation would both extend
	// the young space under it and carve a fresh uninitialised TLAB.
	//
	// THE FRAME IS ANCHORED FIRST, and not because anything allocates -- what
	// the anchor DOES is link the calling method's compiled frames into the
	// chain rootsVisitNativeFrames walks (jit/Jit.h). Without it the walk
	// rewrote the whole heap and every root EXCEPT the frame slots of the
	// method that asked, so `a become: b` left the caller's own `a` naming the
	// object nothing else pointed to any more. Measured: the same-size case
	// passed and this one did not.
	PRIMITIVE_ALLOCATES(args);
	heapFillAllTlabTails(heap);
	BecomeRewrite rewrite = { from, to };

	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &heap->oldSpace, classes);
	for (RawObject *object = pageSpaceIteratorNext(&iterator); object != NULL;
			object = pageSpaceIteratorNext(&iterator)) {
		becomeInObject(object, &rewrite, classes);
	}

	// The young space is a bump region, so it walks from the FIRST OBJECT
	// ADDRESS to the cursor. Every gap is a free chunk by now, and
	// objectSizeInBytes strides over one exactly as it strides over a live
	// object -- which is the whole reason a free chunk IS an object here.
	//
	// nurseryFirstObject and not `fromSpace`: a young object sits at 8 modulo
	// 16 (memory/Nursery.h), so starting at the base read the first header 8
	// bytes early, got a size of zero and stopped -- silently, at the first
	// object, with the entire young generation unwalked. See the note there for
	// what that cost.
	Nursery *nursery = &heap->newSpace;
	for (uint8_t *cursor = nurseryFirstObject(nursery); cursor < nursery->top; ) {
		RawObject *object = (RawObject *) cursor;
		// heapWalkStride and not objectSizeInBytes: a TLAB tail capped by
		// retireTlab is a FREE chunk, and one bigger than the header's size
		// field keeps its length in its BODY (memory/PageSpace.h). Asking the
		// general size function for it reaches for a class index a free chunk
		// deliberately does not have, and aborts on the assertion that exists
		// to catch exactly that read.
		size_t size = heapWalkStride(classes, object);
		if (size == 0) {
			break; // a header that describes nothing: stop rather than loop
		}
		becomeInObject(object, &rewrite, classes);
		cursor += size;
	}

	collectorVisitRoots(heap, becomeRoot, &rewrite);
	PRIMITIVE_DONE_ALLOCATING();
	// Re-read: the argument's own slot was one of the references just rewritten
	// if anything named it, and answering a Value captured before the walk
	// would answer an address the walk may have replaced.
	return primitiveArgument(args, 0);
}
