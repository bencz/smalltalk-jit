// Indexed access.
//
// ONE polymorphic at:, because that is what the kernel declares: AtPrimitive
// sits on Object and String, ByteArray, Array and FloatArray all inherit it.
//
// The storage format answers almost everything, and the one case it cannot is
// String versus ByteArray: both are FORMAT_BYTES, and at: answers a Character
// for one and a SmallInteger for the other. That is decided by CLASS, which is
// the only place in this file where the format is not enough.
//
// Indices are 1-based, and out of range FAILS rather than raising here: the
// Smalltalk fallback is what knows how to signal an IndexOutOfBounds with a
// useful message.
//
// A NOTE ON WHAT IS NOT HERE. This domain used to carry an `indexedTarget`
// helper that decoded receiver, format and index in one call. Nothing ever
// called it -- primAt and primAtPut each do the decoding inline, because each
// wants the format afterwards and the helper threw it away -- and with -Wall off
// nothing said so. The split is what surfaced it, and it was removed rather than
// carried across.

#include "runtime/primitives/Shared.h"
#include <string.h>


// Named instance variables sit between the element count and the indexed
// elements, so an indexed class that also has named slots has its element zero
// past them. Array has none and pays a load it does not need; a wrong answer
// for the classes that do have them would be far more expensive.
static Value *indexedPointerBase(RawObject *object)
{
	RawClass *class = (RawClass *) classTableAt(&CurrentThread.heap->classes,
		rawObjectClassIndex(object));
	return (Value *) object->body + 1 + class->instanceShape.fixedSlots;
}


static _Bool receiverIsString(RawObject *object)
{
	uint32_t index = rawObjectClassIndex(object);
	return (Handles.String.raw != NULL && index == classIndexOf(&Handles.String))
		|| (Handles.Symbol.raw != NULL && index == classIndexOf(&Handles.Symbol));
}


Value primBasicSize(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)) {
		return PRIMITIVE_FAILED; // an immediate has no indexed part
	}
	RawObject *object = asObject(receiver);
	switch (rawObjectFormat(object)) {
	case FORMAT_INDEXED_POINTERS:
	case FORMAT_BYTES:
	case FORMAT_DOUBLES:
		return tagInt((intptr_t) rawObjectElementCount(object));
	case FORMAT_NO_POINTERS:
	case FORMAT_POINTERS:
		return tagInt(0); // not indexed, so it has no indexed elements
	default:
		// The MIXED formats put their count somewhere the format alone does not
		// describe (a class's first body word is its superclass, not a count),
		// so reading one here would answer a tagged pointer as a size.
		return PRIMITIVE_FAILED;
	}
}


Value primAt(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value indexValue = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(indexValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(receiver);
	ObjectFormat format = rawObjectFormat(object);
	if (format != FORMAT_INDEXED_POINTERS && format != FORMAT_BYTES
			&& format != FORMAT_DOUBLES) {
		return PRIMITIVE_FAILED; // not indexed at all
	}
	intptr_t oneBased = asCInt(indexValue);
	if (oneBased < 1 || (size_t) oneBased > rawObjectElementCount(object)) {
		return PRIMITIVE_FAILED; // the fallback raises a useful IndexOutOfBounds
	}
	size_t index = (size_t) oneBased - 1;

	switch (format) {
	case FORMAT_INDEXED_POINTERS:
		return indexedPointerBase(object)[index];
	case FORMAT_DOUBLES:
		FLOAT_RESULT(args, rawObjectDoubles(object)[index]);
	default:
		return receiverIsString(object)
			? tagChar((char) rawObjectBytes(object)[index])
			: tagInt((intptr_t) rawObjectBytes(object)[index]);
	}
}


Value primAtPut(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value indexValue = primitiveArgument(args, 0);
	Value value = primitiveArgument(args, 1);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(indexValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(receiver);
	ObjectFormat format = rawObjectFormat(object);
	intptr_t oneBased = asCInt(indexValue);
	if (format != FORMAT_INDEXED_POINTERS && format != FORMAT_BYTES
			&& format != FORMAT_DOUBLES) {
		return PRIMITIVE_FAILED;
	}
	if (oneBased < 1 || (size_t) oneBased > rawObjectElementCount(object)) {
		return PRIMITIVE_FAILED;
	}
	size_t index = (size_t) oneBased - 1;

	switch (format) {
	case FORMAT_INDEXED_POINTERS:
		// Through the generational write barrier, always. An old Array storing a
		// young pointer without logging it is the bug that survives every test
		// until the young object is collected while still reachable only from
		// old space.
		rawObjectStoreValue(object, &indexedPointerBase(object)[index], value);
		return value;

	case FORMAT_DOUBLES: {
		Number number = numberOf(value);
		if (number.kind == NUM_NOT) {
			return PRIMITIVE_FAILED;
		}
		rawObjectDoubles(object)[index] = number.asFloat;
		return value;
	}

	default:
		if (receiverIsString(object)) {
			if (!valueTypeOf(value, VALUE_CHAR)) {
				return PRIMITIVE_FAILED;
			}
			rawObjectBytes(object)[index] = (uint8_t) (unsigned char) asCChar(value);
			return value;
		}
		if (!valueTypeOf(value, VALUE_INT)) {
			return PRIMITIVE_FAILED;
		}
		intptr_t byte = asCInt(value);
		if (byte < 0 || byte > 255) {
			return PRIMITIVE_FAILED;
		}
		rawObjectBytes(object)[index] = (uint8_t) byte;
		return value;
	}
}


// ---------------------------------------------------------------------------
// Named instance variables, by index
// ---------------------------------------------------------------------------
//
// `instVarAt:` reaches the NAMED slots -- the ones a class declares in its
// `| a b c |` -- and never the indexed elements, which is what `at:` above is
// for. An Array has three named slots and a thousand elements or none at all,
// and the two are numbered separately.
//
// WHERE THEY START DEPENDS ON THE FORMAT and the class agrees with the
// collector's own walk (memory/ObjectWalk.h, objectPointerSlots):
//
//   POINTERS          body word 0, `fixedSlots` of them;
//   INDEXED_POINTERS  body word 1, after the element count, `fixedSlots` of
//                     them, and the elements follow;
//   MIXED_BYTES       after `rawWords`, `pointerWords` of them: a class, whose
//                     named slots are its mirror fields;
//   everything else   NONE. A BoxedFloat64's body word is an IEEE double and a
//                     String's is text; handing either back as a Value would be
//                     a fabricated object pointer, which is the one answer a
//                     reflective accessor must never give.
static Value *namedSlots(RawObject *object, size_t *count)
{
	RawClass *class = (RawClass *) classTableAt(&CurrentThread.heap->classes,
		rawObjectClassIndex(object));
	if (class == NULL) {
		*count = 0;
		return NULL;
	}
	InstanceShape shape = class->instanceShape;
	Value *body = (Value *) object->body;
	switch (rawObjectFormat(object)) {
	case FORMAT_POINTERS:
		*count = shape.fixedSlots;
		return body;
	case FORMAT_INDEXED_POINTERS:
		*count = shape.fixedSlots;
		return body + 1; // past the element count
	case FORMAT_MIXED_BYTES:
		*count = shape.pointerWords;
		return body + shape.rawWords;
	default:
		*count = 0;
		return NULL;
	}
}


// Object>>instVarAt: anInteger
Value primInstVarAt(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value index = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(index, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	size_t count;
	Value *slots = namedSlots(asObject(receiver), &count);
	intptr_t i = asCInt(index);
	if (slots == NULL || i < 1 || (size_t) i > count) {
		return PRIMITIVE_FAILED; // the kernel raises with the honest range
	}
	// THE SLOT, UNTRANSLATED, exactly as `at:` above answers an element and as a
	// compiled GETIVAR reads one.
	//
	// It used to answer nil for anything that was not a pointer, to turn the
	// allocator's zero into the nil Smalltalk promises for an unset variable.
	// That rejected EVERY IMMEDIATE: an instance variable holding a
	// SmallInteger, a Character or a SmallFloat64 read back as nil through this
	// primitive while the same variable read as itself from compiled code.
	// Measured: `Object>>shallowCopy` walks instVarAt:/instVarAt:put:, so
	// `(Dictionary new at: #a put: 1; yourself) copy size` answered nil -- the
	// copy's `tally` was dropped on the way across and the copy claimed to have
	// no size at all.
	//
	// The zero it was defending against cannot reach here from an object the
	// image made: allocateInstance (runtime/primitives/Allocation.c) fills every
	// pointer slot with nil precisely because that is where the Smalltalk rule
	// starts applying, and classFillAbsentSmalltalkFields does the same for the
	// classes the VM builds itself.
	return slots[i - 1];
}


// Object>>instVarAt: anInteger put: anObject
//
// THROUGH THE BARRIER, and that is the whole reason this cannot be done from
// Smalltalk: storing a young object into a slot of an old one has to be
// remembered, and only C is on that side of the line.
Value primInstVarAtPut(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value index = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(index, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(receiver);
	size_t count;
	Value *slots = namedSlots(object, &count);
	intptr_t i = asCInt(index);
	if (slots == NULL || i < 1 || (size_t) i > count) {
		return PRIMITIVE_FAILED;
	}
	Value value = primitiveArgument(args, 1);
	if (valueTypeOf(value, VALUE_POINTER)) {
		rawObjectStorePtr(object, &slots[i - 1], asObject(value));
	} else {
		slots[i - 1] = value;
	}
	return value;
}


// SequenceableCollection>>replaceFrom: start to: stop with: replacement startingAt: repStart
//
// ONE memmove FOR BYTE COLLECTIONS (String, Symbol, ByteArray), which is what
// bulk copying is for; a pointer Array falls through to the kernel's at:put:
// loop, whose stores run the GC write barrier this cannot skip. memmove and
// not memcpy, because `self` and `replacement` may be the SAME object shifted
// (that is how an OrderedCollection of bytes makes room).
//
// The bounds are checked here and a violation FAILS to the fallback, whose
// at:put: raises the same OutOfRangeError a wrong index raises everywhere
// else. An empty range (stop < start) answers the receiver untouched, exactly
// as the fallback's `start to: stop do:` would.
Value primReplaceBytes(Value *args, uint64_t argc)
{
	if (argc != 4) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value startValue = primitiveArgument(args, 0);
	Value stopValue = primitiveArgument(args, 1);
	Value replacementValue = primitiveArgument(args, 2);
	Value replacementStartValue = primitiveArgument(args, 3);
	if (!valueTypeOf(receiver, VALUE_POINTER)
			|| !valueTypeOf(startValue, VALUE_INT)
			|| !valueTypeOf(stopValue, VALUE_INT)
			|| !valueTypeOf(replacementStartValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(receiver);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED; // a pointer Array copies through the barrier
	}
	intptr_t start = asCInt(startValue);
	intptr_t stop = asCInt(stopValue);
	intptr_t replacementStart = asCInt(replacementStartValue);
	if (stop < start) {
		return receiver; // empty range: the fallback loop would not run either
	}
	if (!valueTypeOf(replacementValue, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *replacement = asObject(replacementValue);
	if (rawObjectFormat(replacement) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	size_t count = (size_t) (stop - start + 1);
	if (start < 1 || replacementStart < 1
			|| (size_t) stop > rawObjectElementCount(object)
			|| (size_t) (replacementStart - 1) + count
				> rawObjectElementCount(replacement)) {
		return PRIMITIVE_FAILED;
	}
	memmove(rawObjectBytes(object) + (start - 1),
		rawObjectBytes(replacement) + (replacementStart - 1), count);
	return receiver;
}
