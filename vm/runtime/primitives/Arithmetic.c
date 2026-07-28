// Arithmetic on SmallIntegers and Floats.
//
// NONE of these allocates, and that is a design decision rather than an
// omission: a result that would need a heap box -- an overflowed sum, a double
// outside the SmallFloat64 window, a Fraction -- FAILS, and the method's own
// Smalltalk code builds it where allocating is ordinary. That is what lets the
// whole domain skip PRIMITIVE_ALLOCATES.
//
// IntAddPrimitive and FloatAddPrimitive name the SAME function here, and so do
// the other three pairs. It is free, and it is exactly the mixed-arithmetic fast
// path the old VM did not have and paid 100x for.

#include "runtime/primitives/Shared.h"


Value primAdd(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t sum;
		if (__builtin_add_overflow(asCInt(receiver), asCInt(argument), &sum)
				|| !smallIntFits(sum)) {
			return PRIMITIVE_FAILED; // LargeInteger is the fallback's business
		}
		return tagInt(sum);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat + b.asFloat);
}


Value primSubtract(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t difference;
		if (__builtin_sub_overflow(asCInt(receiver), asCInt(argument), &difference)
				|| !smallIntFits(difference)) {
			return PRIMITIVE_FAILED;
		}
		return tagInt(difference);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat - b.asFloat);
}


Value primMultiply(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t product;
		if (__builtin_mul_overflow(asCInt(receiver), asCInt(argument), &product)
				|| !smallIntFits(product)) {
			return PRIMITIVE_FAILED;
		}
		return tagInt(product);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat * b.asFloat);
}


// Integer division answers an integer ONLY when it is exact. 7/2 fails, and the
// Smalltalk fallback answers a Fraction. Answering 3 here would be a wrong
// answer that no test of the primitive itself could catch, because 3 is a
// perfectly plausible SmallInteger.
Value primDivide(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t divisor = asCInt(argument);
		if (divisor == 0) {
			return PRIMITIVE_FAILED; // ZeroDivide is raised in Smalltalk
		}
		intptr_t dividend = asCInt(receiver);
		if (dividend % divisor != 0) {
			return PRIMITIVE_FAILED; // a Fraction, which allocates
		}
		intptr_t quotient = dividend / divisor;
		return smallIntFits(quotient) ? tagInt(quotient) : PRIMITIVE_FAILED;
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	if (b.asFloat == 0.0) {
		// The quotient would be an infinity or a NaN, neither of which fits an
		// immediate, so this is the same answer floatResult would give. Said
		// here so the intent is division by zero and not a representation
		// accident.
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat / b.asFloat);
}


// Floor division and floor modulo, SmallInteger only.
//
// C division truncates toward zero; Smalltalk's // and \\ round toward
// negative infinity, so -7 // 2 is -4 and -7 \\ 2 is 1. The correction below is
// the whole difference, and getting it wrong is a wrong answer for exactly the
// negative operands that tests forget.
//
// Float receivers deliberately FAIL here rather than being handled: doing them
// needs floor() from libm, and the Smalltalk fallback is the designed home for
// the general case. That is a scope decision, not an oversight.
Value primFloorDivide(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t divisor = asCInt(argument);
	if (divisor == 0) {
		return PRIMITIVE_FAILED;
	}
	intptr_t dividend = asCInt(receiver);
	intptr_t quotient = dividend / divisor;
	if (dividend % divisor != 0 && ((dividend < 0) != (divisor < 0))) {
		quotient--;
	}
	return smallIntFits(quotient) ? tagInt(quotient) : PRIMITIVE_FAILED;
}


Value primFloorModulo(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t divisor = asCInt(argument);
	if (divisor == 0) {
		return PRIMITIVE_FAILED;
	}
	intptr_t remainder = asCInt(receiver) % divisor;
	if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
		remainder += divisor;
	}
	// |remainder| < |divisor| <= 2^61, so it always fits.
	return tagInt(remainder);
}


// quo: and rem: TRUNCATE toward zero, which is what C division already does, so
// they are the pair that needs no correction. // and \\ are the ones that round
// toward negative infinity; keeping both pairs is not redundancy, it is the
// difference between -7 quo: 2 = -3 and -7 // 2 = -4.
Value primQuotient(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument) || asCInt(argument) == 0) {
		return PRIMITIVE_FAILED;
	}
	intptr_t quotient = asCInt(receiver) / asCInt(argument);
	return smallIntFits(quotient) ? tagInt(quotient) : PRIMITIVE_FAILED;
}


Value primRemainder(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument) || asCInt(argument) == 0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt(asCInt(receiver) % asCInt(argument));
}


Value primNegated(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	// minVal has no positive counterpart: the payload range is asymmetric, so
	// negating it overflows and the LargeInteger fallback takes over.
	intptr_t negated = -asCInt(receiver);
	return smallIntFits(negated) ? tagInt(negated) : PRIMITIVE_FAILED;
}


Value primAsFloat(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	return floatResult((double) asCInt(receiver));
}
