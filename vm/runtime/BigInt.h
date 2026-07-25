#ifndef BIGINT_H
#define BIGINT_H

// Arbitrary-precision integer MAGNITUDE kernels.
//
// Deliberately knows nothing about the VM: no Value, no RawObject, no handles,
// no allocation of Smalltalk objects. A magnitude is a plain array of 32-bit
// limbs, LEAST SIGNIFICANT FIRST, normalized so the top limb is nonzero (a
// length of 0 IS the value zero). Sign lives with the caller, matching the
// image side where the sign is the concrete class.
//
// That separation is the point: these routines can be exercised, and their
// endian independence proved, by a standalone self-test (ST_BIGINT_TEST) before
// any Smalltalk representation depends on them.
//
// WHY 32-BIT LIMBS. A 64-bit limb needs a 128-bit intermediate for multiply and
// division, which means __int128, which is a compiler extension this VM does
// not otherwise rely on and which the ppc64 bring-up would have to re-validate.
// 32-bit limbs multiply into a uint64_t, which is standard C. The cost is about
// a factor of two on the widest operations, against an asymptotic improvement
// of a much larger factor over what the image does today.
//
// Every routine writes into a caller-provided output buffer whose required
// capacity is documented per function, and answers the NORMALIZED length. The
// only routines that allocate are the ones with a divide-and-conquer scratch
// need (multiply above the Karatsuba threshold, division, radix conversion),
// which malloc internally and free before returning.

#include <stddef.h>
#include <stdint.h>

// Strip trailing zero limbs; answers the normalized length.
size_t bigIntNormalize(const uint32_t *a, size_t n);

// -1, 0 or 1 comparing MAGNITUDES. Inputs must be normalized.
int bigIntCompare(const uint32_t *a, size_t na, const uint32_t *b, size_t nb);

// out needs max(na, nb) + 1 limbs.
size_t bigIntAdd(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);

// Requires a >= b as magnitudes. out needs na limbs.
size_t bigIntSub(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);

// out needs na + nb limbs, and must not alias a or b.
size_t bigIntMul(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);

// Truncating division of magnitudes. q needs na limbs, r needs nb limbs.
// Answers 0 when nb == 0 (division by zero), leaving the outputs untouched.
_Bool bigIntDivMod(const uint32_t *a, size_t na, const uint32_t *b, size_t nb,
	uint32_t *q, size_t *nq, uint32_t *r, size_t *nr);

// out needs min(na, nb) limbs.
size_t bigIntGcd(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out);

// out needs na + bits/32 + 1 limbs.
size_t bigIntShiftLeft(const uint32_t *a, size_t na, size_t bits, uint32_t *out);

// out needs na limbs. Shifts the MAGNITUDE, so a caller wanting Smalltalk's
// floored semantics for a negative value must adjust (see LargeInteger).
size_t bigIntShiftRight(const uint32_t *a, size_t na, size_t bits, uint32_t *out);

// Position of the highest set bit, 1-based; 0 for a zero magnitude.
size_t bigIntHighBit(const uint32_t *a, size_t na);

// Position of the lowest set bit, 1-based; 0 for a zero magnitude.
size_t bigIntLowBit(const uint32_t *a, size_t na);

// Number of set bits in the magnitude.
size_t bigIntBitCount(const uint32_t *a, size_t na);

// Bit at a 0-based position of the magnitude.
int bigIntBitAt(const uint32_t *a, size_t na, size_t index);

// Correctly rounded (round half to even) conversion of the magnitude to a
// double; answers HUGE_VAL when it does not fit. The image's version was a
// Horner loop in doubles, which accumulates error and is not correctly rounded.
double bigIntToDouble(const uint32_t *a, size_t na);

// Decimal or other radix (2..36) rendering, WITHOUT a sign. Answers the number
// of characters written, or 0 if the buffer is too small. out is NOT
// null-terminated. bigIntStringSize gives a safe upper bound.
size_t bigIntToString(const uint32_t *a, size_t na, int base, char *out, size_t outSize);
size_t bigIntStringSize(size_t na, int base);

// Parse digits in the given radix (2..36); ignores nothing, so the caller must
// pass only digit characters. out needs bigIntFromStringSize limbs.
size_t bigIntFromString(const char *digits, size_t len, int base, uint32_t *out);
size_t bigIntFromStringSize(size_t len, int base);

#endif
