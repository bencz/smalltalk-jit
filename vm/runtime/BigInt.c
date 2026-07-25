// Arbitrary-precision magnitude kernels. See BigInt.h for the representation
// and for why the limbs are 32 bits.
//
// Endianness: a limb array is a sequence of uint32_t VALUES, never a byte
// image, so every routine here is endian independent by construction. The
// byte order only becomes visible when the image stores limbs in a byte
// payload, which is why ST_BIGINT_TEST runs on both POWER byte orders as well
// as x86: it proves the arithmetic, and the storage layer is proved separately.
#include "runtime/BigInt.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LIMB_BITS 32
#define LIMB_MASK 0xFFFFFFFFu

// Below this many limbs, schoolbook multiplication wins: Karatsuba's three
// recursive products plus their additions and the scratch buffer cost more
// than the n*m inner loop saves. The classic crossover is a few tens of limbs;
// tuning it is a measurement, not a correctness question.
#define KARATSUBA_THRESHOLD 40


size_t bigIntNormalize(const uint32_t *a, size_t n)
{
	while (n > 0 && a[n - 1] == 0) {
		n--;
	}
	return n;
}


int bigIntCompare(const uint32_t *a, size_t na, const uint32_t *b, size_t nb)
{
	if (na != nb) {
		return na < nb ? -1 : 1;
	}
	for (size_t i = na; i > 0; i--) {
		if (a[i - 1] != b[i - 1]) {
			return a[i - 1] < b[i - 1] ? -1 : 1;
		}
	}
	return 0;
}


size_t bigIntAdd(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out)
{
	if (na < nb) {
		const uint32_t *ta = a; a = b; b = ta;
		size_t tn = na; na = nb; nb = tn;
	}
	uint64_t carry = 0;
	for (size_t i = 0; i < na; i++) {
		uint64_t sum = (uint64_t) a[i] + carry + (i < nb ? b[i] : 0);
		out[i] = (uint32_t) (sum & LIMB_MASK);
		carry = sum >> LIMB_BITS;
	}
	out[na] = (uint32_t) carry;
	return bigIntNormalize(out, na + 1);
}


size_t bigIntSub(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out)
{
	ASSERT(bigIntCompare(a, na, b, nb) >= 0);
	int64_t borrow = 0;
	for (size_t i = 0; i < na; i++) {
		int64_t diff = (int64_t) a[i] - borrow - (int64_t) (i < nb ? b[i] : 0);
		if (diff < 0) {
			diff += ((int64_t) 1 << LIMB_BITS);
			borrow = 1;
		} else {
			borrow = 0;
		}
		out[i] = (uint32_t) diff;
	}
	ASSERT(borrow == 0);
	return bigIntNormalize(out, na);
}


static void bigIntMulSchoolbook(const uint32_t *a, size_t na, const uint32_t *b, size_t nb,
	uint32_t *out)
{
	memset(out, 0, (na + nb) * sizeof(uint32_t));
	for (size_t i = 0; i < na; i++) {
		uint64_t carry = 0;
		uint64_t ai = a[i];
		if (ai == 0) {
			continue;
		}
		for (size_t j = 0; j < nb; j++) {
			uint64_t cur = (uint64_t) out[i + j] + ai * b[j] + carry;
			out[i + j] = (uint32_t) (cur & LIMB_MASK);
			carry = cur >> LIMB_BITS;
		}
		// Propagate past the inner loop; a single limb of carry can ripple.
		size_t k = i + nb;
		while (carry != 0) {
			uint64_t cur = (uint64_t) out[k] + carry;
			out[k] = (uint32_t) (cur & LIMB_MASK);
			carry = cur >> LIMB_BITS;
			k++;
		}
	}
}


// In-place add of `src` into `dst` at limb offset `offset`, with carry ripple.
// Used to accumulate Karatsuba's three partial products.
static void bigIntAddInto(uint32_t *dst, size_t ndst, const uint32_t *src, size_t nsrc,
	size_t offset)
{
	uint64_t carry = 0;
	size_t i = 0;
	for (; i < nsrc; i++) {
		ASSERT(offset + i < ndst);
		uint64_t cur = (uint64_t) dst[offset + i] + src[i] + carry;
		dst[offset + i] = (uint32_t) (cur & LIMB_MASK);
		carry = cur >> LIMB_BITS;
	}
	size_t k = offset + i;
	while (carry != 0) {
		ASSERT(k < ndst);
		uint64_t cur = (uint64_t) dst[k] + carry;
		dst[k] = (uint32_t) (cur & LIMB_MASK);
		carry = cur >> LIMB_BITS;
		k++;
	}
}


// In-place subtract of `src` from `dst` at limb offset `offset`.
static void bigIntSubFrom(uint32_t *dst, size_t ndst, const uint32_t *src, size_t nsrc,
	size_t offset)
{
	int64_t borrow = 0;
	size_t i = 0;
	for (; i < nsrc; i++) {
		ASSERT(offset + i < ndst);
		int64_t diff = (int64_t) dst[offset + i] - borrow - (int64_t) src[i];
		if (diff < 0) {
			diff += ((int64_t) 1 << LIMB_BITS);
			borrow = 1;
		} else {
			borrow = 0;
		}
		dst[offset + i] = (uint32_t) diff;
	}
	size_t k = offset + i;
	while (borrow != 0) {
		ASSERT(k < ndst);
		int64_t diff = (int64_t) dst[k] - borrow;
		if (diff < 0) {
			diff += ((int64_t) 1 << LIMB_BITS);
			borrow = 1;
		} else {
			borrow = 0;
		}
		dst[k] = (uint32_t) diff;
		k++;
	}
}


// Karatsuba: split both operands at the same limb offset and trade one of the
// four sub-products for two additions.
//   a = a1*B + a0, b = b1*B + b0
//   z0 = a0*b0, z2 = a1*b1, z1 = (a0+a1)*(b0+b1) - z0 - z2
//   a*b = z2*B^2 + z1*B + z0
static void bigIntMulKaratsuba(const uint32_t *a, size_t na, const uint32_t *b, size_t nb,
	uint32_t *out)
{
	size_t half = (na > nb ? na : nb + 0) / 2;
	if (half == 0 || na <= KARATSUBA_THRESHOLD || nb <= KARATSUBA_THRESHOLD) {
		bigIntMulSchoolbook(a, na, b, nb, out);
		return;
	}

	size_t na0 = na < half ? na : half;
	size_t na1 = na > half ? na - half : 0;
	size_t nb0 = nb < half ? nb : half;
	size_t nb1 = nb > half ? nb - half : 0;
	const uint32_t *a0 = a, *a1 = a + na0;
	const uint32_t *b0 = b, *b1 = b + nb0;
	na0 = bigIntNormalize(a0, na0);
	nb0 = bigIntNormalize(b0, nb0);
	na1 = bigIntNormalize(a1, na1);
	nb1 = bigIntNormalize(b1, nb1);

	size_t nsa = (na0 > na1 ? na0 : na1) + 1;
	size_t nsb = (nb0 > nb1 ? nb0 : nb1) + 1;
	// sumA, sumB, and z1 (whose product needs nsa + nsb limbs).
	uint32_t *scratch = calloc(nsa + nsb + nsa + nsb, sizeof(uint32_t));
	if (scratch == NULL) {
		FAIL();
	}
	uint32_t *sumA = scratch;
	uint32_t *sumB = sumA + nsa;
	uint32_t *z1 = sumB + nsb;

	size_t nSumA = bigIntAdd(a0, na0, a1, na1, sumA);
	size_t nSumB = bigIntAdd(b0, nb0, b1, nb1, sumB);

	memset(out, 0, (na + nb) * sizeof(uint32_t));
	// z0 and z2 go straight into their final positions in out.
	if (na0 > 0 && nb0 > 0) {
		uint32_t *z0 = calloc(na0 + nb0, sizeof(uint32_t));
		if (z0 == NULL) {
			FAIL();
		}
		bigIntMulKaratsuba(a0, na0, b0, nb0, z0);
		size_t nz0 = bigIntNormalize(z0, na0 + nb0);
		bigIntAddInto(out, na + nb, z0, nz0, 0);
		bigIntMulKaratsuba(sumA, nSumA, sumB, nSumB, z1);
		size_t nz1 = bigIntNormalize(z1, nsa + nsb);
		// z1 -= z0 in place, then subtract z2 after computing it.
		uint32_t *tmp = calloc(nsa + nsb, sizeof(uint32_t));
		if (tmp == NULL) {
			FAIL();
		}
		memcpy(tmp, z1, (nsa + nsb) * sizeof(uint32_t));
		bigIntSubFrom(tmp, nsa + nsb, z0, nz0, 0);
		free(z0);
		if (na1 > 0 && nb1 > 0) {
			uint32_t *z2 = calloc(na1 + nb1, sizeof(uint32_t));
			if (z2 == NULL) {
				FAIL();
			}
			bigIntMulKaratsuba(a1, na1, b1, nb1, z2);
			size_t nz2 = bigIntNormalize(z2, na1 + nb1);
			bigIntAddInto(out, na + nb, z2, nz2, 2 * half);
			bigIntSubFrom(tmp, nsa + nsb, z2, nz2, 0);
			free(z2);
		}
		size_t nTmp = bigIntNormalize(tmp, nsa + nsb);
		bigIntAddInto(out, na + nb, tmp, nTmp, half);
		free(tmp);
		(void) nz1;
	} else {
		// One half is empty: nothing to gain, fall back.
		free(scratch);
		bigIntMulSchoolbook(a, na, b, nb, out);
		return;
	}
	free(scratch);
}


size_t bigIntMul(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out)
{
	if (na == 0 || nb == 0) {
		return 0;
	}
	if (na <= KARATSUBA_THRESHOLD || nb <= KARATSUBA_THRESHOLD) {
		bigIntMulSchoolbook(a, na, b, nb, out);
	} else {
		bigIntMulKaratsuba(a, na, b, nb, out);
	}
	return bigIntNormalize(out, na + nb);
}


// Knuth TAOCP 4.3.1 Algorithm D. The image did BIT BY BIT long division,
// allocating a fresh limb array per bit, so a single division of an n-bit
// number was O(n) allocations; this is O(nq * nb) limb operations and
// allocates one scratch buffer.
static _Bool bigIntDivModKnuth(const uint32_t *a, size_t na, const uint32_t *b, size_t nb,
	uint32_t *q, size_t *nq, uint32_t *r, size_t *nr)
{
	// Normalize so the divisor's top limb has its high bit set, which is what
	// makes the two-limb quotient estimate correct to within one.
	int shift = 0;
	uint32_t top = b[nb - 1];
	while ((top & 0x80000000u) == 0) {
		top <<= 1;
		shift++;
	}

	uint32_t *un = calloc(na + 1, sizeof(uint32_t));
	uint32_t *vn = calloc(nb, sizeof(uint32_t));
	if (un == NULL || vn == NULL) {
		FAIL();
	}
	if (shift == 0) {
		memcpy(un, a, na * sizeof(uint32_t));
		memcpy(vn, b, nb * sizeof(uint32_t));
		un[na] = 0;
	} else {
		for (size_t i = nb; i > 0; i--) {
			vn[i - 1] = (b[i - 1] << shift)
				| (i >= 2 ? (uint32_t) (b[i - 2] >> (LIMB_BITS - shift)) : 0);
		}
		un[na] = (uint32_t) (a[na - 1] >> (LIMB_BITS - shift));
		for (size_t i = na; i > 0; i--) {
			un[i - 1] = (a[i - 1] << shift)
				| (i >= 2 ? (uint32_t) (a[i - 2] >> (LIMB_BITS - shift)) : 0);
		}
	}

	uint64_t base = (uint64_t) 1 << LIMB_BITS;
	for (size_t j = na - nb + 1; j > 0; j--) {
		size_t jj = j - 1;
		uint64_t numerator = ((uint64_t) un[jj + nb] << LIMB_BITS) | un[jj + nb - 1];
		uint64_t qhat = numerator / vn[nb - 1];
		uint64_t rhat = numerator % vn[nb - 1];
		while (qhat >= base
				|| (nb >= 2 && qhat * vn[nb - 2] > ((rhat << LIMB_BITS) | un[jj + nb - 2]))) {
			qhat--;
			rhat += vn[nb - 1];
			if (rhat >= base) {
				break;
			}
		}

		// Multiply and subtract, then add back on the rare overshoot.
		int64_t borrow = 0;
		uint64_t carry = 0;
		for (size_t i = 0; i < nb; i++) {
			uint64_t p = qhat * vn[i] + carry;
			carry = p >> LIMB_BITS;
			int64_t t = (int64_t) un[jj + i] - (int64_t) (p & LIMB_MASK) - borrow;
			if (t < 0) {
				t += (int64_t) base;
				borrow = 1;
			} else {
				borrow = 0;
			}
			un[jj + i] = (uint32_t) t;
		}
		int64_t t = (int64_t) un[jj + nb] - (int64_t) carry - borrow;
		if (t < 0) {
			t += (int64_t) base;
			borrow = 1;
		} else {
			borrow = 0;
		}
		un[jj + nb] = (uint32_t) t;

		if (borrow != 0) {
			// qhat was one too large: undo by adding the divisor back.
			qhat--;
			uint64_t addCarry = 0;
			for (size_t i = 0; i < nb; i++) {
				uint64_t sum = (uint64_t) un[jj + i] + vn[i] + addCarry;
				un[jj + i] = (uint32_t) (sum & LIMB_MASK);
				addCarry = sum >> LIMB_BITS;
			}
			un[jj + nb] = (uint32_t) (un[jj + nb] + addCarry);
		}
		q[jj] = (uint32_t) qhat;
	}

	// Denormalize the remainder.
	if (shift == 0) {
		for (size_t i = 0; i < nb; i++) {
			r[i] = un[i];
		}
	} else {
		for (size_t i = 0; i < nb; i++) {
			r[i] = (un[i] >> shift)
				| (i + 1 < nb + 1 ? (uint32_t) (un[i + 1] << (LIMB_BITS - shift)) : 0);
		}
	}

	*nq = bigIntNormalize(q, na - nb + 1);
	*nr = bigIntNormalize(r, nb);
	free(un);
	free(vn);
	return 1;
}


_Bool bigIntDivMod(const uint32_t *a, size_t na, const uint32_t *b, size_t nb,
	uint32_t *q, size_t *nq, uint32_t *r, size_t *nr)
{
	if (nb == 0) {
		return 0;
	}
	if (bigIntCompare(a, na, b, nb) < 0) {
		*nq = 0;
		memcpy(r, a, na * sizeof(uint32_t));
		*nr = na;
		return 1;
	}
	if (nb == 1) {
		// Single-limb divisor: the general path's normalization is pure
		// overhead, and this case is the inner loop of radix printing.
		uint64_t divisor = b[0];
		uint64_t rem = 0;
		for (size_t i = na; i > 0; i--) {
			uint64_t cur = (rem << LIMB_BITS) | a[i - 1];
			q[i - 1] = (uint32_t) (cur / divisor);
			rem = cur % divisor;
		}
		*nq = bigIntNormalize(q, na);
		r[0] = (uint32_t) rem;
		*nr = rem == 0 ? 0 : 1;
		return 1;
	}
	return bigIntDivModKnuth(a, na, b, nb, q, nq, r, nr);
}


size_t bigIntShiftLeft(const uint32_t *a, size_t na, size_t bits, uint32_t *out)
{
	if (na == 0) {
		return 0;
	}
	size_t limbShift = bits / LIMB_BITS;
	size_t bitShift = bits % LIMB_BITS;
	size_t total = na + limbShift + 1;
	memset(out, 0, total * sizeof(uint32_t));
	if (bitShift == 0) {
		memcpy(out + limbShift, a, na * sizeof(uint32_t));
	} else {
		for (size_t i = 0; i < na; i++) {
			out[limbShift + i] |= a[i] << bitShift;
			out[limbShift + i + 1] = a[i] >> (LIMB_BITS - bitShift);
		}
	}
	return bigIntNormalize(out, total);
}


size_t bigIntShiftRight(const uint32_t *a, size_t na, size_t bits, uint32_t *out)
{
	size_t limbShift = bits / LIMB_BITS;
	if (na == 0 || limbShift >= na) {
		return 0;
	}
	size_t bitShift = bits % LIMB_BITS;
	size_t total = na - limbShift;
	if (bitShift == 0) {
		memcpy(out, a + limbShift, total * sizeof(uint32_t));
	} else {
		for (size_t i = 0; i < total; i++) {
			out[i] = a[limbShift + i] >> bitShift;
			if (limbShift + i + 1 < na) {
				out[i] |= a[limbShift + i + 1] << (LIMB_BITS - bitShift);
			}
		}
	}
	return bigIntNormalize(out, total);
}


size_t bigIntHighBit(const uint32_t *a, size_t na)
{
	if (na == 0) {
		return 0;
	}
	uint32_t top = a[na - 1];
	size_t bit = 0;
	while (top != 0) {
		top >>= 1;
		bit++;
	}
	return (na - 1) * LIMB_BITS + bit;
}


size_t bigIntLowBit(const uint32_t *a, size_t na)
{
	for (size_t i = 0; i < na; i++) {
		if (a[i] != 0) {
			uint32_t limb = a[i];
			size_t bit = 1;
			while ((limb & 1) == 0) {
				limb >>= 1;
				bit++;
			}
			return i * LIMB_BITS + bit;
		}
	}
	return 0;
}


size_t bigIntBitCount(const uint32_t *a, size_t na)
{
	size_t count = 0;
	for (size_t i = 0; i < na; i++) {
		uint32_t limb = a[i];
		while (limb != 0) {
			count += limb & 1;
			limb >>= 1;
		}
	}
	return count;
}


int bigIntBitAt(const uint32_t *a, size_t na, size_t index)
{
	size_t limb = index / LIMB_BITS;
	if (limb >= na) {
		return 0;
	}
	return (a[limb] >> (index % LIMB_BITS)) & 1;
}


// Binary GCD (Stein): shifts and subtractions only, no division at all. The
// image used Euclid through \\, so every step paid a full long division.
size_t bigIntGcd(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out)
{
	if (na == 0) {
		memcpy(out, b, nb * sizeof(uint32_t));
		return nb;
	}
	if (nb == 0) {
		memcpy(out, a, na * sizeof(uint32_t));
		return na;
	}

	// All three buffers are the same generous size: u and v are swapped and
	// written into each other throughout, so sizing them separately would make
	// a shift result overflow the smaller one.
	size_t cap = (na > nb ? na : nb) + 1;
	uint32_t *u = calloc(cap, sizeof(uint32_t));
	uint32_t *v = calloc(cap, sizeof(uint32_t));
	uint32_t *t = calloc(cap, sizeof(uint32_t));
	if (u == NULL || v == NULL || t == NULL) {
		FAIL();
	}
	memcpy(u, a, na * sizeof(uint32_t));
	memcpy(v, b, nb * sizeof(uint32_t));
	size_t nu = na, nv = nb;

	// Factor out the power of two they share, then make both odd: from here
	// every difference is even and loses at least one bit per round.
	size_t shiftU = bigIntLowBit(u, nu) - 1;
	size_t shiftV = bigIntLowBit(v, nv) - 1;
	size_t common = shiftU < shiftV ? shiftU : shiftV;
	nu = bigIntShiftRight(u, nu, shiftU, t);
	memcpy(u, t, nu * sizeof(uint32_t));
	nv = bigIntShiftRight(v, nv, shiftV, t);
	memcpy(v, t, nv * sizeof(uint32_t));

	// Invariant: u and v are both odd and nonzero, and gcd(u, v) is unchanged.
	// Order so u <= v, replace v by (v - u) >> twos, and stop when they meet.
	while (1) {
		if (bigIntCompare(u, nu, v, nv) > 0) {
			uint32_t *swap = u; u = v; v = swap;
			size_t ns = nu; nu = nv; nv = ns;
		}
		size_t nt = bigIntSub(v, nv, u, nu, t);
		if (nt == 0) {
			break; // u == v, and THAT is the gcd
		}
		size_t low = bigIntLowBit(t, nt) - 1;
		nv = bigIntShiftRight(t, nt, low, v);
	}

	size_t result = bigIntShiftLeft(u, nu, common, out);
	free(u);
	free(v);
	free(t);
	return result;
}


double bigIntToDouble(const uint32_t *a, size_t na)
{
	size_t bits = bigIntHighBit(a, na);
	if (bits == 0) {
		return 0.0;
	}
	if (bits > 1024) {
		return HUGE_VAL;
	}

	// Take the top 54 bits: 53 of mantissa plus one to round on, and remember
	// whether anything below them was set (the sticky bit). Rounding half to
	// even off that is exactly what the hardware would do, which a Horner loop
	// in doubles is not.
	uint64_t mantissa = 0;
	size_t taken = 0;
	size_t bit = bits;
	while (taken < 54 && bit > 0) {
		bit--;
		mantissa = (mantissa << 1) | (uint64_t) bigIntBitAt(a, na, bit);
		taken++;
	}
	_Bool sticky = 0;
	for (size_t i = bit; i > 0; i--) {
		if (bigIntBitAt(a, na, i - 1)) {
			sticky = 1;
			break;
		}
	}

	size_t exponent = bits - taken;
	if (taken == 54) {
		uint64_t roundBit = mantissa & 1;
		mantissa >>= 1;
		exponent += 1;
		if (roundBit && (sticky || (mantissa & 1))) {
			mantissa++;
			if (mantissa == ((uint64_t) 1 << 53)) { // carried out of 53 bits
				mantissa >>= 1;
				exponent++;
			}
		}
	}
	return ldexp((double) mantissa, (int) exponent);
}


size_t bigIntStringSize(size_t na, int base)
{
	// Every limb contributes at most 32 bits, and every output digit carries at
	// least one bit, so 32 characters per limb is always enough; +1 for safety.
	(void) base;
	return na * LIMB_BITS + 1;
}


size_t bigIntToString(const uint32_t *a, size_t na, int base, char *out, size_t outSize)
{
	static const char Digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	ASSERT(base >= 2 && base <= 36);
	if (na == 0) {
		if (outSize < 1) {
			return 0;
		}
		out[0] = '0';
		return 1;
	}
	if (outSize < bigIntStringSize(na, base)) {
		return 0;
	}

	// Divide by the largest power of the base that fits a limb, so one
	// division yields many digits instead of one. The image divided by 10000
	// each time through a general division; this both takes bigger bites and
	// uses the single-limb fast path.
	uint32_t chunkBase = (uint32_t) base;
	int chunkDigits = 1;
	while ((uint64_t) chunkBase * (uint64_t) base <= 0xFFFFFFFFull) {
		chunkBase *= (uint32_t) base;
		chunkDigits++;
	}

	uint32_t *work = calloc(na, sizeof(uint32_t));
	uint32_t *quotient = calloc(na, sizeof(uint32_t));
	if (work == NULL || quotient == NULL) {
		FAIL();
	}
	memcpy(work, a, na * sizeof(uint32_t));
	size_t nw = na;

	size_t written = 0;
	while (nw != 0) {
		uint64_t rem = 0;
		for (size_t i = nw; i > 0; i--) {
			uint64_t cur = (rem << LIMB_BITS) | work[i - 1];
			quotient[i - 1] = (uint32_t) (cur / chunkBase);
			rem = cur % chunkBase;
		}
		size_t nq = bigIntNormalize(quotient, nw);
		memcpy(work, quotient, nq * sizeof(uint32_t));
		nw = nq;
		if (nw != 0) {
			// More limbs follow, so this chunk is INTERIOR and must be padded
			// to its full width: dropping its leading zeros here would splice
			// the digits above and below it together.
			for (int d = 0; d < chunkDigits; d++) {
				out[written++] = Digits[rem % (uint32_t) base];
				rem /= (uint32_t) base;
			}
		} else {
			// Final chunk: emit only its significant digits, which is what
			// suppresses the leading zeros of the whole number.
			while (rem != 0) {
				out[written++] = Digits[rem % (uint32_t) base];
				rem /= (uint32_t) base;
			}
		}
	}

	// The digits came out least significant first.
	for (size_t i = 0; i < written / 2; i++) {
		char tmp = out[i];
		out[i] = out[written - 1 - i];
		out[written - 1 - i] = tmp;
	}
	free(work);
	free(quotient);
	return written;
}


size_t bigIntFromStringSize(size_t len, int base)
{
	// Each digit adds at most ceil(log2(base)) bits, which is the bit length of
	// base-1. Using a flat 6 for every base would be safe but wildly loose for
	// binary (six times the limbs actually needed), and bigIntFromString
	// MEMSETS the whole span it is told about, so a loose bound is not merely
	// wasteful: it writes past a buffer a caller sized by the real need.
	ASSERT(base >= 2 && base <= 36);
	size_t bitsPerDigit = 0;
	for (int v = base - 1; v != 0; v >>= 1) {
		bitsPerDigit++;
	}
	return (len * bitsPerDigit + LIMB_BITS - 1) / LIMB_BITS + 1;
}


size_t bigIntFromString(const char *digits, size_t len, int base, uint32_t *out)
{
	ASSERT(base >= 2 && base <= 36);
	size_t cap = bigIntFromStringSize(len, base);
	memset(out, 0, cap * sizeof(uint32_t));
	size_t n = 0;

	// Same chunking as printing, in reverse: fold as many digits as fit a limb
	// into one multiply-and-add.
	uint32_t chunkBase = (uint32_t) base;
	int chunkDigits = 1;
	while ((uint64_t) chunkBase * (uint64_t) base <= 0xFFFFFFFFull) {
		chunkBase *= (uint32_t) base;
		chunkDigits++;
	}

	size_t pos = 0;
	while (pos < len) {
		size_t take = len - pos < (size_t) chunkDigits ? len - pos : (size_t) chunkDigits;
		uint32_t chunk = 0;
		uint32_t scale = 1;
		for (size_t i = 0; i < take; i++) {
			char c = digits[pos + i];
			int d;
			if (c >= '0' && c <= '9') {
				d = c - '0';
			} else if (c >= 'a' && c <= 'z') {
				d = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'Z') {
				d = c - 'A' + 10;
			} else {
				d = 0;
			}
			chunk = chunk * (uint32_t) base + (uint32_t) d;
			scale *= (uint32_t) base;
		}
		pos += take;

		uint64_t carry = chunk;
		for (size_t i = 0; i < n; i++) {
			uint64_t cur = (uint64_t) out[i] * scale + carry;
			out[i] = (uint32_t) (cur & LIMB_MASK);
			carry = cur >> LIMB_BITS;
		}
		while (carry != 0) {
			ASSERT(n < cap);
			out[n++] = (uint32_t) (carry & LIMB_MASK);
			carry >>= LIMB_BITS;
		}
	}
	return bigIntNormalize(out, n);
}
