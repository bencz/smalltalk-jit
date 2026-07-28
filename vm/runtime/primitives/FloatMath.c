// Float mathematics.
//
// Every one of these was DECLARED with an empty body, so before this each one
// answered its receiver and a program that took a square root carried on with
// the number it started from. They are batched because they are the same three
// lines each: classify the receiver, call libm, fail if the result does not fit
// the immediate window so the kernel's own code can box it.
//
// FAILING ON A NON-FLOAT RECEIVER IS THE POINT of numberOf answering NUM_NOT:
// `4 sqrt` has an Integer receiver and belongs to Number's Smalltalk code, not
// here.
//
// This is the file that makes every level linking these sources need -lm.

#include "runtime/primitives/Shared.h"
#include "runtime/String.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Written out per primitive rather than through a shared helper, because
// FLOAT_RESULT anchors the frame with __builtin_return_address(0) and that has
// to be taken in the primitive itself (runtime/Primitive.h).
#define FLOAT_UNARY(name, fn) \
	Value name(Value *args, uint64_t argc) \
	{ \
		if (argc != 0) { \
			return PRIMITIVE_FAILED; \
		} \
		Number self = numberOf(primitiveReceiver(args)); \
		if (self.kind == NUM_NOT) { \
			return PRIMITIVE_FAILED; \
		} \
		FLOAT_RESULT(args, fn(self.asFloat)); \
	}

FLOAT_UNARY(primFloatSqrt, sqrt)
FLOAT_UNARY(primFloatSin, sin)
FLOAT_UNARY(primFloatCos, cos)
FLOAT_UNARY(primFloatTan, tan)
FLOAT_UNARY(primFloatArcSin, asin)
FLOAT_UNARY(primFloatArcCos, acos)
FLOAT_UNARY(primFloatArcTan, atan)
FLOAT_UNARY(primFloatExp, exp)
FLOAT_UNARY(primFloatLn, log)


Value primFloatArcTan2(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	Number other = numberOf(primitiveArgument(args, 0));
	if (self.kind == NUM_NOT || other.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	FLOAT_RESULT(args, atan2(self.asFloat, other.asFloat));
}


// A whole double as a SmallInteger, or failure.
//
// The window is the tagged one and NOT the double's: a finite double of
// magnitude 2^53 or more is already integral and the kernel answers it by a
// different route, so overflowing here is an ordinary fall-through and not an
// error. Written as a double comparison rather than a cast, because casting a
// double outside the integer range is undefined behaviour in C and would be a
// wrong answer rather than a failure.
//
// THE BOUNDS COME FROM THE ONE PLACE THAT HAS THEM. They used to be written out
// here as +-2^62, which is the range the payload would have if it were
// UNSIGNED; it is signed, so the real window is [-2^61, 2^61-1], and the
// literals let through everything between 2^61 and 2^62. tagInt asserts that
// range, so `(1.0 timesTwoPower: 61) truncated` did not answer a
// LargePositiveInteger -- it ABORTED THE VM. That is the asymmetry
// runtime/primitives/Shared.h states in capitals, got wrong by a factor of two.
//
// The upper test is `<` against 2^61 rather than `<=` against 2^61-1 on purpose:
// 2^61-1 has no exact double, so the nearest one is 2^61 and `<=` would let 2^61
// itself through again. -2^61 IS exact and IS a SmallInteger, so the lower test
// is inclusive.
static Value integerResult(double value)
{
	double smallest = (double) SMALL_INT_MIN;    // -2^61, exact
	double justPastLargest = -(double) SMALL_INT_MIN; // 2^61, exact
	if (!(value >= smallest && value < justPastLargest)) {
		return PRIMITIVE_FAILED; // NaN and infinity land here too, by !(...)
	}
	return tagInt((intptr_t) value);
}


#define FLOAT_TO_INTEGER(name, fn) \
	Value name(Value *args, uint64_t argc) \
	{ \
		if (argc != 0) { \
			return PRIMITIVE_FAILED; \
		} \
		Number self = numberOf(primitiveReceiver(args)); \
		if (self.kind == NUM_NOT) { \
			return PRIMITIVE_FAILED; \
		} \
		return integerResult(fn(self.asFloat)); \
	}

FLOAT_TO_INTEGER(primFloatFloor, floor)
FLOAT_TO_INTEGER(primFloatCeiling, ceil)
FLOAT_TO_INTEGER(primFloatTruncated, trunc)
// round() and NOT nearbyint(). Smalltalk's `rounded` breaks ties AWAY FROM
// ZERO, so 0.5 is 1 and 2.5 is 3; nearbyint follows the current rounding mode,
// which is ties-to-EVEN by default, so it answered 0 and 2. It is the same
// thing said in two places disagreeing: packages/Core/src/Magnitudes/Number.st
// documents `rounded` as "ties away from zero, matching the C round() behind
// Float>>rounded", and the C behind it was not round().
FLOAT_TO_INTEGER(primFloatRounded, round)


// Base-2 exponent: 1.0 answers 0, 0.5 answers -1. Zero, infinity and NaN have
// none, and the kernel raises for them, so this fails on all three.
Value primFloatExponent(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	if (self.kind == NUM_NOT || !isfinite(self.asFloat) || self.asFloat == 0.0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) ilogb(self.asFloat));
}


// self * 2^n, EXACT: ldexp touches the exponent and leaves the mantissa alone,
// which is what `asExactedFraction` depends on to recover the mantissa bits.
Value primFloatTimesTwoPower(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	Value power = primitiveArgument(args, 0);
	if (self.kind == NUM_NOT || !valueTypeOf(power, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t n = asCInt(power);
	if (n < INT_MIN || n > INT_MAX) {
		return PRIMITIVE_FAILED;
	}
	FLOAT_RESULT(args, ldexp(self.asFloat, (int) n));
}


// A Float as the SHORTEST decimal that reads back as the same double.
//
// Shortest-round-trip and not a fixed precision, because both fixed choices are
// wrong in a way a user sees: `%.17g` prints 3.14 as 3.1400000000000001, and
// anything shorter stops round-tripping, so `x printString asNumber = x` becomes
// false for ordinary values. The loop is the standard way to get it without a
// Grisu implementation: ask for one significant digit, read it back, and widen
// until it matches. Seventeen always matches, so it terminates.
//
// The trailing `.0` is Smalltalk's, not C's: a Float that happens to be integral
// still prints as a Float, or `1500.0 printString` would answer '1500' and the
// value would read back as an Integer.
Value primFloatAsString(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	if (self.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	double value = self.asFloat;

	char buffer[64];
	if (isnan(value)) {
		snprintf(buffer, sizeof buffer, "nan");
	} else if (isinf(value)) {
		snprintf(buffer, sizeof buffer, value < 0 ? "-inf" : "inf");
	} else {
		// `%e` and not `%g` to FIND the digit count, because %g also decides the
		// notation, and it decides it by a rule nobody here wants: at two
		// significant digits it renders 1500.0 as "1.5e+03", which round-trips
		// and is the wrong answer for `1500.0 printString`.
		int digits = 17;
		for (int d = 1; d <= 17; d++) {
			snprintf(buffer, sizeof buffer, "%.*e", d - 1, value);
			if (strtod(buffer, NULL) == value) {
				digits = d;
				break;
			}
		}
		// The notation is then chosen SEPARATELY, on the decimal exponent that
		// %e just reported. Plain for everything a reader would write plainly,
		// scientific only where plain would be a wall of zeros.
		const char *marker = strchr(buffer, 'e');
		long exponent10 = marker == NULL ? 0 : strtol(marker + 1, NULL, 10);
		if (exponent10 >= -5 && exponent10 <= 15) {
			int decimals = (int) (digits - 1 - exponent10);
			snprintf(buffer, sizeof buffer, "%.*f", decimals < 0 ? 0 : decimals,
				value);
		}
		// A bare digit string has to read back as a Float, so it gets ".0"; a
		// scientific mantissa with no point gets one before the exponent.
		if (strpbrk(buffer, ".n") == NULL) {
			char *exponent = strpbrk(buffer, "eE");
			if (exponent == NULL) {
				size_t length = strlen(buffer);
				if (length + 3 <= sizeof buffer) {
					memcpy(buffer + length, ".0", 3);
				}
			} else {
				char tail[64];
				snprintf(tail, sizeof tail, "%s", exponent);
				size_t head = (size_t) (exponent - buffer);
				if (head + 2 + strlen(tail) + 1 <= sizeof buffer) {
					memcpy(buffer + head, ".0", 2);
					memcpy(buffer + head + 2, tail, strlen(tail) + 1);
				}
			}
		}
	}

	// Allocates, so the caller's compiled frames are anchored first.
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(buffer));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
