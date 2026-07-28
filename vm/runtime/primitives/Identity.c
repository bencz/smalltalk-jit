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
