// Numeric comparison.
//
// One shape for all six. The integer pair compares payloads directly; a mixed
// pair goes through double, which loses precision above 2^53 and can call two
// distinct large integers equal. Accepted for now and written down rather than
// discovered: the exact mixed comparison belongs with the numeric tower, which
// is fallback code.

#include "runtime/primitives/Shared.h"

typedef enum {
	CMP_LESS,
	CMP_GREATER,
	CMP_LESS_EQUAL,
	CMP_GREATER_EQUAL,
	CMP_EQUAL,
	CMP_NOT_EQUAL,
} Comparison;


static Value compareNumbers(Value *args, uint64_t argc, Comparison how)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	_Bool answer;
	if (bothSmallIntegers(receiver, argument)) {
		intptr_t a = asCInt(receiver);
		intptr_t b = asCInt(argument);
		switch (how) {
		case CMP_LESS: answer = a < b; break;
		case CMP_GREATER: answer = a > b; break;
		case CMP_LESS_EQUAL: answer = a <= b; break;
		case CMP_GREATER_EQUAL: answer = a >= b; break;
		case CMP_EQUAL: answer = a == b; break;
		default: answer = a != b; break;
		}
		return booleanResult(answer);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		// `3 = aString` must answer false rather than fail, but that is
		// Object>>= 's job: this primitive is installed on the numeric classes
		// and its contract is "compare two numbers". A non-number argument is
		// precisely the case the fallback exists for.
		return PRIMITIVE_FAILED;
	}
	switch (how) {
	case CMP_LESS: answer = a.asFloat < b.asFloat; break;
	case CMP_GREATER: answer = a.asFloat > b.asFloat; break;
	case CMP_LESS_EQUAL: answer = a.asFloat <= b.asFloat; break;
	case CMP_GREATER_EQUAL: answer = a.asFloat >= b.asFloat; break;
	case CMP_EQUAL: answer = a.asFloat == b.asFloat; break;
	default: answer = a.asFloat != b.asFloat; break;
	}
	return booleanResult(answer);
}


// ALL SIX ARE PRIMITIVES NOW, and the line that used to be here -- "only `<` and
// `=`, because Magnitude derives the rest in Smalltalk" -- described a cost
// rather than a design. Measured, on the float benchmark: `x > 1000.0` reached
// Float>>> , which sends `isKindOf:`, then `asFloat`, then `<`, so ONE source
// comparison was four sends and two method activations. `primIdentical`,
// `classOf` and `primClass` together held 11% of that benchmark's cycles and
// every one of those samples came from a comparison walking a class chain.
//
// Deriving them in Smalltalk is still what happens for every OTHER Magnitude.
// What changed is that the two classes on the arithmetic fast path stopped
// paying for the derivation.
Value primLess(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_LESS); }
Value primNumericEqual(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_EQUAL); }
Value primGreater(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_GREATER); }
Value primLessEqual(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_LESS_EQUAL); }
Value primGreaterEqual(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_GREATER_EQUAL); }
Value primNumericNotEqual(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_NOT_EQUAL); }
