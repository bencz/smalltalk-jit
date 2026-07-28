// Bit operations on SmallIntegers.

#include "runtime/primitives/Shared.h"

typedef enum { BIT_AND, BIT_OR, BIT_XOR } BitOperation;


static Value bitOperation(Value *args, uint64_t argc, BitOperation how)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	// The tag is 00 for both, so the operation applies to the tagged Values
	// directly and the result is already tagged. That is true of and, or and
	// xor, and it is NOT true of addition, which is why only these three take
	// the shortcut.
	switch (how) {
	case BIT_AND: return receiver & argument;
	case BIT_OR: return receiver | argument;
	default: return receiver ^ argument;
	}
}


Value primBitAnd(Value *a, uint64_t n) { return bitOperation(a, n, BIT_AND); }
Value primBitOr(Value *a, uint64_t n) { return bitOperation(a, n, BIT_OR); }
Value primBitXor(Value *a, uint64_t n) { return bitOperation(a, n, BIT_XOR); }


// Positive shifts left, negative shifts right, and a left shift that would
// leave the SmallInteger range fails rather than truncating.
Value primBitShift(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t value = asCInt(receiver);
	intptr_t shift = asCInt(argument);

	if (shift >= 0) {
		if (shift >= SMALL_INT_PAYLOAD_BITS) {
			return value == 0 ? tagInt(0) : PRIMITIVE_FAILED;
		}
		// Shift as UNSIGNED: left-shifting a negative signed value is undefined
		// behaviour, and the round trip below is what detects the bits lost.
		intptr_t shifted = (intptr_t) ((uintptr_t) value << shift);
		if ((shifted >> shift) != value || !smallIntFits(shifted)) {
			return PRIMITIVE_FAILED;
		}
		return tagInt(shifted);
	}

	// An arithmetic right shift, saturating: shifting a 62-bit value right by
	// more than its width leaves 0 or -1, and letting the shift count reach the
	// register width would be undefined behaviour instead.
	uintptr_t amount = (uintptr_t) -shift;
	if (amount >= SMALL_INT_PAYLOAD_BITS) {
		return tagInt(value < 0 ? -1 : 0);
	}
	return tagInt(value >> amount);
}
