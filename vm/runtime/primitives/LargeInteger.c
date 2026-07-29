// LargeInteger over the magnitude kernels in runtime/BigInt.c.
//
// THE KERNELS WERE ALREADY HERE. BigInt.c -- Karatsuba multiply, Knuth
// algorithm D division, binary GCD, chunked radix conversion -- is in the build
// and has its own self-test; what was missing was the six primitives that hand
// it operands and turn its answers back into objects. Until they existed, every
// path in packages/Core/src/Magnitudes/LargeInteger.st fell through to
// `self error: 'large integer arithmetic failed'`, which was the single most
// common failure in the suite.
//
// REPRESENTATION, and it is the .st file's, not this file's: a LargeInteger is
// a BytesShape object whose byte payload IS the magnitude, 32-bit limbs least
// significant first in native order. The SIGN IS THE CONCRETE CLASS
// (LargePositiveInteger / LargeNegativeInteger), Pharo style, so there is no
// sign field that can disagree with the value.
//
// NORMALIZATION HAPPENS HERE, on every result: a magnitude that fits the 62-bit
// signed payload comes back as a tagged SmallInteger, so a live LargeInteger is
// always genuinely too big to be immediate. The threshold is ASYMMETRIC,
// [-2^61, 2^61-1], and it has to agree with SmallInteger class maxVal/minVal,
// with tagInt's assertion and with the parser's literal builder, or the same
// magnitude gets a different class depending on how it was produced.
//
// THE TWO CLASSES ARRIVE AS AN ARGUMENT, a 2-element Array, rather than through
// Handles. LargeInteger.st says why: adding a Handles entry is append-only and
// bumps the snapshot format, and a by-name global lookup per operation would be
// a hash probe on the hot path. The kernel holds the pair in a class variable.

#include "runtime/primitives/Shared.h"
#include "runtime/BigInt.h"
#include "runtime/Collection.h"
#include "runtime/String.h"
#include <stdlib.h>
#include <string.h>

#define LARGE_LIMB_BITS 32

// The opcodes. THEY ARE PART OF THE CONTRACT and are not to be renumbered:
// LargeInteger.st sends them as literal integers and says so at the sends.
enum {
	LARGE_OP_ADD = 1,
	LARGE_OP_SUB = 2,
	LARGE_OP_MUL = 3,
	LARGE_OP_QUO = 4,   // truncating quotient
	LARGE_OP_REM = 5,   // remainder of QUO, sign of the dividend
	LARGE_OP_DIV = 6,   // floored quotient
	LARGE_OP_MOD = 7,   // remainder of DIV, sign of the divisor
	LARGE_OP_GCD = 8,
};


// Read a SmallInteger or a LargeInteger as (limbs, count). The SmallInteger
// case materialises into the caller's 2-limb scratch, and that is what lets
// every operation below take a MIXED pair without the image converting first.
static _Bool largeOperand(Value value, uint32_t *scratch, const uint32_t **limbs,
	size_t *count)
{
	if (valueTypeOf(value, VALUE_INT)) {
		intptr_t i = asCInt(value);
		// Negate through uint64_t: the magnitude of the most negative value is
		// one past the positive range, so negating in intptr_t would overflow.
		uint64_t magnitude = i < 0 ? (uint64_t) (-(int64_t) i) : (uint64_t) i;
		scratch[0] = (uint32_t) (magnitude & 0xFFFFFFFFu);
		scratch[1] = (uint32_t) (magnitude >> LARGE_LIMB_BITS);
		*limbs = scratch;
		*count = bigIntNormalize(scratch, 2);
		return 1;
	}
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0; // a Character or a Float: the kernel's coercion handles those
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return 0;
	}
	size_t bytes = rawObjectElementCount(object);
	if (bytes % sizeof(uint32_t) != 0) {
		return 0; // a String, say: byte-shaped but not a limb array
	}
	*limbs = (const uint32_t *) rawObjectBytes(object);
	*count = bigIntNormalize(*limbs, bytes / sizeof(uint32_t));
	return 1;
}


// The classes pair, validated. Answers 0 for anything that is not a 2-element
// Array, because everything below indexes it.
static _Bool largeClasses(Value value, RawArray **classes)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_INDEXED_POINTERS
			|| rawObjectElementCount(object) != 2) {
		return 0;
	}
	*classes = (RawArray *) object;
	return 1;
}


// Is this operand negative? SEPARATE from largeOperand because the answer for a
// heap operand is its CLASS, which only the caller's classes pair names.
static _Bool largeNegative(Value value, RawArray *classes)
{
	if (valueTypeOf(value, VALUE_INT)) {
		return asCInt(value) < 0;
	}
	Value negativeClass = classes->vars[1];
	if (!valueTypeOf(negativeClass, VALUE_POINTER)) {
		return 0;
	}
	// BY CLASS INDEX and not by object identity: the index is what an object's
	// header carries (ADR 0005), so this is a compare of two integers rather
	// than a dereference of the class.
	return rawObjectClassIndex(asObject(value))
		== ((RawClass *) asObject(negativeClass))->classIndex;
}


// Does this magnitude fit the 62-bit SIGNED payload? Asymmetric on purpose:
// -2^61 is a SmallInteger and +2^61 is not.
static _Bool fitsSmallInteger(const uint32_t *limbs, size_t n, _Bool negative)
{
	if (n > 2) {
		return 0;
	}
	uint64_t magnitude = 0;
	if (n >= 1) {
		magnitude |= limbs[0];
	}
	if (n == 2) {
		magnitude |= (uint64_t) limbs[1] << LARGE_LIMB_BITS;
	}
	uint64_t limit = (uint64_t) 1 << 61;
	return negative ? magnitude <= limit : magnitude < limit;
}


// The image-level result: a tagged SmallInteger when it fits, otherwise a fresh
// byte-shaped instance of the sign's concrete class.
//
// ALLOCATES, so every caller anchors its frame before reaching here. `limbs`
// points into a malloc'd buffer rather than into the heap, which is why only
// the class needs a handle across the allocation.
static Value largeResult(const uint32_t *limbs, size_t n, _Bool negative,
	RawArray *classes)
{
	n = bigIntNormalize(limbs, n);
	if (n == 0) {
		return tagInt(0); // zero has no sign
	}
	if (fitsSmallInteger(limbs, n, negative)) {
		uint64_t magnitude = limbs[0];
		if (n == 2) {
			magnitude |= (uint64_t) limbs[1] << LARGE_LIMB_BITS;
		}
		return tagInt(negative ? -(intptr_t) magnitude : (intptr_t) magnitude);
	}
	HandleScope scope;
	openHandleScope(&scope);
	Class *class = scopeHandle(asObject(classes->vars[negative ? 1 : 0]));
	Object *result = newObject(class, n * sizeof(uint32_t));
	memcpy(rawObjectBytes(result->raw), limbs, n * sizeof(uint32_t));
	Value tagged = objectTagged(result);
	closeHandleScope(&scope, NULL);
	return tagged;
}


// Integer>>primLargeWith: aNumber op: opcode classes: classPair
Value primLargeIntBinary(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	Value opcode = primitiveArgument(args, 1);
	RawArray *classes;
	if (!valueTypeOf(opcode, VALUE_INT)
			|| !largeClasses(primitiveArgument(args, 2), &classes)) {
		return PRIMITIVE_FAILED;
	}
	uint32_t scratchA[2], scratchB[2];
	const uint32_t *a, *b;
	size_t na, nb;
	if (!largeOperand(primitiveReceiver(args), scratchA, &a, &na)
			|| !largeOperand(primitiveArgument(args, 0), scratchB, &b, &nb)) {
		return PRIMITIVE_FAILED;
	}
	_Bool negA = largeNegative(primitiveReceiver(args), classes);
	_Bool negB = largeNegative(primitiveArgument(args, 0), classes);
	intptr_t op = asCInt(opcode);

	// Worst case across every operation: a product needs na + nb limbs, a sum
	// needs max + 1, and the floored forms carry one more.
	size_t capacity = na + nb + 4;
	uint32_t *out = calloc(capacity, sizeof(uint32_t));
	if (out == NULL) {
		return PRIMITIVE_FAILED;
	}
	size_t nOut = 0;
	_Bool negOut = 0;
	_Bool ok = 1;

	switch (op) {
	case LARGE_OP_ADD:
	case LARGE_OP_SUB: {
		// Subtraction is addition with the second sign flipped, so one branch
		// serves both and the sign logic exists once.
		_Bool effectiveB = op == LARGE_OP_SUB ? !negB : negB;
		if (negA == effectiveB) {
			nOut = bigIntAdd(a, na, b, nb, out);
			negOut = negA;
		} else if (bigIntCompare(a, na, b, nb) >= 0) {
			nOut = bigIntSub(a, na, b, nb, out);
			negOut = negA;
		} else {
			nOut = bigIntSub(b, nb, a, na, out);
			negOut = effectiveB;
		}
		break;
	}

	case LARGE_OP_MUL:
		nOut = bigIntMul(a, na, b, nb, out);
		negOut = negA != negB;
		break;

	case LARGE_OP_QUO:
	case LARGE_OP_REM:
	case LARGE_OP_DIV:
	case LARGE_OP_MOD: {
		if (nb == 0) {
			ok = 0; // division by zero: the kernel raises ZeroDivide
			break;
		}
		uint32_t *quotient = calloc(na + 1, sizeof(uint32_t));
		uint32_t *remainder = calloc(nb + 1, sizeof(uint32_t));
		if (quotient == NULL || remainder == NULL) {
			free(quotient);
			free(remainder);
			ok = 0;
			break;
		}
		size_t nq = 0, nr = 0;
		bigIntDivMod(a, na, b, nb, quotient, &nq, remainder, &nr);
		_Bool negQ = negA != negB;
		if (op == LARGE_OP_QUO) {
			memcpy(out, quotient, nq * sizeof(uint32_t));
			nOut = nq;
			negOut = negQ;
		} else if (op == LARGE_OP_REM) {
			memcpy(out, remainder, nr * sizeof(uint32_t));
			nOut = nr;
			negOut = negA; // a remainder takes the DIVIDEND's sign
		} else {
			// The FLOORED forms. When the signs differ and the division was not
			// exact, the quotient goes one further from zero and the remainder
			// swings to the divisor's sign. Getting this wrong is a wrong answer
			// for exactly the negative operands tests forget.
			_Bool adjust = negQ && nr != 0;
			if (op == LARGE_OP_DIV) {
				if (adjust) {
					uint32_t one = 1;
					nOut = bigIntAdd(quotient, nq, &one, 1, out);
				} else {
					memcpy(out, quotient, nq * sizeof(uint32_t));
					nOut = nq;
				}
				negOut = negQ;
			} else {
				if (adjust) {
					nOut = bigIntSub(b, nb, remainder, nr, out);
				} else {
					memcpy(out, remainder, nr * sizeof(uint32_t));
					nOut = nr;
				}
				negOut = negB; // a modulo takes the DIVISOR's sign
			}
		}
		free(quotient);
		free(remainder);
		break;
	}

	case LARGE_OP_GCD:
		nOut = bigIntGcd(a, na, b, nb, out);
		negOut = 0; // a gcd is non-negative
		break;

	default:
		ok = 0;
		break;
	}

	Value result = PRIMITIVE_FAILED;
	if (ok) {
		PRIMITIVE_ALLOCATES(args);
		result = largeResult(out, nOut, negOut, classes);
		PRIMITIVE_DONE_ALLOCATING();
	}
	free(out);
	return result;
}


// Integer>>primLargeCompareWith: aNumber classes: classPair
//
// Answers -1, 0 or 1. Nothing allocates: the answer is always an immediate.
Value primLargeIntCompare(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	RawArray *classes;
	if (!largeClasses(primitiveArgument(args, 1), &classes)) {
		return PRIMITIVE_FAILED;
	}
	uint32_t scratchA[2], scratchB[2];
	const uint32_t *a, *b;
	size_t na, nb;
	if (!largeOperand(primitiveReceiver(args), scratchA, &a, &na)
			|| !largeOperand(primitiveArgument(args, 0), scratchB, &b, &nb)) {
		return PRIMITIVE_FAILED;
	}
	_Bool negA = largeNegative(primitiveReceiver(args), classes);
	_Bool negB = largeNegative(primitiveArgument(args, 0), classes);
	// ZERO IS NOT NEGATIVE, whatever the class says. Without this, a zero
	// reached through the negative class would compare as less than itself.
	if (na == 0) {
		negA = 0;
	}
	if (nb == 0) {
		negB = 0;
	}
	int result;
	if (negA != negB) {
		result = negA ? -1 : 1;
	} else {
		result = bigIntCompare(a, na, b, nb);
		if (negA) {
			result = -result;
		}
	}
	return tagInt(result);
}


// Integer>>primLargeShift: anInteger classes: classPair
//
// ARITHMETIC shift. A negative count on a NEGATIVE receiver has to FLOOR, and
// sign-magnitude does not do that for free: shifting the magnitude truncates
// toward zero, so one is subtracted when any set bit was shifted out.
Value primLargeIntShift(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	Value shiftValue = primitiveArgument(args, 0);
	RawArray *classes;
	if (!valueTypeOf(shiftValue, VALUE_INT)
			|| !largeClasses(primitiveArgument(args, 1), &classes)) {
		return PRIMITIVE_FAILED;
	}
	uint32_t scratch[2];
	const uint32_t *a;
	size_t na;
	if (!largeOperand(primitiveReceiver(args), scratch, &a, &na)) {
		return PRIMITIVE_FAILED;
	}
	_Bool negative = largeNegative(primitiveReceiver(args), classes);
	intptr_t shift = asCInt(shiftValue);
	// A shift wide enough to overflow the allocation below is a request for an
	// object bigger than the machine has; refusing beats calloc answering NULL
	// after computing a nonsense size.
	if (shift > (intptr_t) (1 << 24)) {
		return PRIMITIVE_FAILED;
	}

	size_t capacity = na + (shift > 0 ? (size_t) shift / LARGE_LIMB_BITS : 0) + 4;
	uint32_t *out = calloc(capacity, sizeof(uint32_t));
	if (out == NULL) {
		return PRIMITIVE_FAILED;
	}
	size_t nOut;
	if (shift >= 0) {
		nOut = bigIntShiftLeft(a, na, (size_t) shift, out);
	} else {
		size_t amount = (size_t) (-shift);
		nOut = bigIntShiftRight(a, na, amount, out);
		if (negative) {
			_Bool lost = 0;
			for (size_t bit = 0; bit < amount && bit < na * LARGE_LIMB_BITS; bit++) {
				if (bigIntBitAt(a, na, bit)) {
					lost = 1;
					break;
				}
			}
			if (lost) {
				uint32_t one = 1;
				uint32_t *bumped = calloc(nOut + 2, sizeof(uint32_t));
				if (bumped == NULL) {
					free(out);
					return PRIMITIVE_FAILED;
				}
				size_t nBumped = bigIntAdd(out, nOut, &one, 1, bumped);
				memcpy(out, bumped, nBumped * sizeof(uint32_t));
				nOut = nBumped;
				free(bumped);
			}
		}
	}
	PRIMITIVE_ALLOCATES(args);
	Value result = largeResult(out, nOut, negative, classes);
	PRIMITIVE_DONE_ALLOCATING();
	free(out);
	return result;
}


// LargeInteger>>primAsFloatWithClasses: classPair
//
// Correctly rounded in C (round half to even). The kernel's note says the old
// version was a Horner loop in doubles, which accumulates error and silently
// produced Infinity above 2^1024 instead of answering it deliberately.
Value primLargeIntAsFloat(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	RawArray *classes;
	if (!largeClasses(primitiveArgument(args, 0), &classes)) {
		return PRIMITIVE_FAILED;
	}
	uint32_t scratch[2];
	const uint32_t *a;
	size_t na;
	if (!largeOperand(primitiveReceiver(args), scratch, &a, &na)) {
		return PRIMITIVE_FAILED;
	}
	_Bool negative = largeNegative(primitiveReceiver(args), classes);
	double value = bigIntToDouble(a, na);
	FLOAT_RESULT(args, negative ? -value : value);
}


// LargeInteger>>digitsStringBase: anInteger
//
// DIGITS ONLY, without a sign: the kernel prepends '-' itself, so one primitive
// serves printString, printString: and printOn:base:.
Value primLargeIntPrintString(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value baseValue = primitiveArgument(args, 0);
	if (!valueTypeOf(baseValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t base = asCInt(baseValue);
	if (base < 2 || base > 36) {
		return PRIMITIVE_FAILED;
	}
	uint32_t scratch[2];
	const uint32_t *a;
	size_t na;
	if (!largeOperand(primitiveReceiver(args), scratch, &a, &na)) {
		return PRIMITIVE_FAILED;
	}
	// INTO A MALLOC'D BUFFER FIRST, and only then into a String. The conversion
	// reads the limbs, and the limbs live in the heap object; allocating the
	// String could move it.
	size_t capacity = bigIntStringSize(na, (int) base);
	char *buffer = malloc(capacity);
	if (buffer == NULL) {
		return PRIMITIVE_FAILED;
	}
	size_t length = bigIntToString(a, na, (int) base, buffer, capacity);
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	String *string = newString(length);
	memcpy(string->raw->contents, buffer, length);
	Value result = objectTagged(string);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	free(buffer);
	return result;
}


// String>>largeIntegerBase: anInteger negative: aBoolean classes: classPair
//
// THE RECEIVER IS THE DIGIT BUFFER, which is why this lives on String: the
// primitive reads bytes, and LargeInteger class>>fromString:base:negative: is
// the readable entry point that supplies the classes pair.
Value primLargeIntFromString(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value baseValue = primitiveArgument(args, 0);
	RawArray *classes;
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(baseValue, VALUE_INT)
			|| !largeClasses(primitiveArgument(args, 2), &classes)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t base = asCInt(baseValue);
	if (base < 2 || base > 36) {
		return PRIMITIVE_FAILED;
	}
	RawObject *digits = asObject(receiver);
	if (rawObjectFormat(digits) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	size_t length = rawObjectElementCount(digits);
	size_t capacity = bigIntFromStringSize(length, (int) base);
	uint32_t *out = calloc(capacity == 0 ? 1 : capacity, sizeof(uint32_t));
	if (out == NULL) {
		return PRIMITIVE_FAILED;
	}
	size_t n = bigIntFromString((const char *) rawObjectBytes(digits), length,
		(int) base, out);
	_Bool negative = primitiveArgument(args, 1) == tagPtr(Handles.true_.raw);
	PRIMITIVE_ALLOCATES(args);
	Value result = largeResult(out, n, negative, classes);
	PRIMITIVE_DONE_ALLOCATING();
	free(out);
	return result;
}
