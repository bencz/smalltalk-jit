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
		return floatResult(rawObjectDoubles(object)[index]);
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
