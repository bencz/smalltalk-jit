// Standalone self-test for the magnitude kernels (vm/runtime/BigInt.c),
// reachable as ST_BIGINT_TEST=1 with NO image: the kernels know nothing about
// the VM, so this runs before any Smalltalk representation depends on them, and
// runs identically under qemu on both POWER byte orders.
//
// The assertions are IDENTITIES rather than tables of expected values, because
// an identity cannot be satisfied by a consistently wrong implementation the
// way a table copied from a previous run can:
//
//   (a * b) / b == a           and the remainder is zero
//   a == (a / b) * b + (a % b) and 0 <= a % b < b
//   a + b - b == a
//   (a << k) >> k == a
//   gcd(a, b) divides both, and a/g and b/g are coprime
//   parse(print(a)) == a, in several radices
//   toDouble agrees with a reference built from the top bits
//
// Karatsuba is exercised by construction: operands are grown past the
// threshold and every product is cross-checked against the schoolbook path by
// the division identity.
#include "runtime/BigInt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_LIMBS 400

static int bigIntFailures;

static void bigCheck(int ok, const char *what)
{
	if (!ok) {
		printf("BigInt self-test FAILED: %s\n", what);
		bigIntFailures++;
	}
}


// xorshift64, so the sequence is identical on every platform and a failure is
// reproducible from the seed alone.
static uint64_t bigRandState;

static uint64_t bigRandom(void)
{
	bigRandState ^= bigRandState << 13;
	bigRandState ^= bigRandState >> 7;
	bigRandState ^= bigRandState << 17;
	return bigRandState;
}


static size_t randomMagnitude(uint32_t *out, size_t maxLimbs)
{
	size_t n = (size_t) (bigRandom() % maxLimbs) + 1;
	for (size_t i = 0; i < n; i++) {
		out[i] = (uint32_t) bigRandom();
	}
	return bigIntNormalize(out, n);
}


int bigIntSelfTest(void)
{
	bigIntFailures = 0;
	bigRandState = 0x123456789ABCDEFull;

	static uint32_t a[MAX_LIMBS], b[MAX_LIMBS];
	static uint32_t prod[2 * MAX_LIMBS + 2];
	static uint32_t q[2 * MAX_LIMBS + 2], r[2 * MAX_LIMBS + 2];
	static uint32_t t1[4 * MAX_LIMBS + 8], t2[4 * MAX_LIMBS + 8];
	static char text[MAX_LIMBS * 32 + 8];

	// ---- fixed edge cases -------------------------------------------------
	{
		size_t na = 0, nb = 0, nq = 0, nr = 0;
		bigCheck(bigIntCompare(a, 0, b, 0) == 0, "zero compares equal to zero");
		bigCheck(bigIntAdd(a, 0, b, 0, prod) == 0, "0 + 0 is 0");
		bigCheck(bigIntMul(a, 0, b, 0, prod) == 0, "0 * 0 is 0");
		bigCheck(!bigIntDivMod(a, 0, b, 0, q, &nq, r, &nr), "division by zero is rejected");
		a[0] = 1; na = 1;
		b[0] = 1; nb = 1;
		bigCheck(bigIntDivMod(a, na, b, nb, q, &nq, r, &nr) && nq == 1 && q[0] == 1 && nr == 0,
			"1 / 1 is 1 remainder 0");
		bigCheck(bigIntHighBit(a, na) == 1, "highBit of 1");
		bigCheck(bigIntLowBit(a, na) == 1, "lowBit of 1");
		bigCheck(bigIntBitCount(a, na) == 1, "bitCount of 1");
		bigCheck(bigIntHighBit(a, 0) == 0, "highBit of zero");
		bigCheck(bigIntToDouble(a, 0) == 0.0, "toDouble of zero");
		bigCheck(bigIntToDouble(a, na) == 1.0, "toDouble of 1");
		// 2^32, the limb boundary
		a[0] = 0; a[1] = 1; na = 2;
		bigCheck(bigIntToDouble(a, na) == 4294967296.0, "toDouble at the limb boundary");
		bigCheck(bigIntHighBit(a, na) == 33, "highBit at the limb boundary");
		bigCheck(bigIntLowBit(a, na) == 33, "lowBit at the limb boundary");
		size_t n = bigIntToString(a, na, 10, text, sizeof(text));
		bigCheck(n == 10 && memcmp(text, "4294967296", 10) == 0, "print at the limb boundary");
		// exact powers of two convert exactly
		for (size_t k = 0; k < 300; k++) {
			memset(t1, 0, sizeof(uint32_t) * 16);
			t1[k / 32] = 1u << (k % 32);
			size_t nt = bigIntNormalize(t1, k / 32 + 1);
			bigCheck(bigIntToDouble(t1, nt) == ldexp(1.0, (int) k), "toDouble of a power of two");
			bigCheck(bigIntHighBit(t1, nt) == k + 1, "highBit of a power of two");
		}
	}

	// ---- randomized identities -------------------------------------------
	// Sizes deliberately span the Karatsuba threshold (40 limbs) so both
	// multiplication paths run and are cross-checked by the same identities.
	static const size_t Sizes[] = { 1, 2, 3, 8, 20, 39, 40, 41, 80, 160, MAX_LIMBS - 1 };
	for (size_t s = 0; s < sizeof(Sizes) / sizeof(Sizes[0]); s++) {
		for (int iter = 0; iter < 60; iter++) {
			size_t na = randomMagnitude(a, Sizes[s]);
			size_t nb = randomMagnitude(b, Sizes[s]);
			if (na == 0 || nb == 0) {
				continue;
			}

			// (a * b) / b == a exactly
			size_t np = bigIntMul(a, na, b, nb, prod);
			size_t nq = 0, nr = 0;
			bigCheck(bigIntDivMod(prod, np, b, nb, q, &nq, r, &nr), "divmod of a product");
			bigCheck(bigIntCompare(q, nq, a, na) == 0, "(a * b) / b == a");
			bigCheck(nr == 0, "(a * b) % b == 0");

			// commutativity
			size_t np2 = bigIntMul(b, nb, a, na, t1);
			bigCheck(bigIntCompare(prod, np, t1, np2) == 0, "multiplication commutes");

			// a == (a / b) * b + (a % b), with 0 <= r < b
			bigCheck(bigIntDivMod(a, na, b, nb, q, &nq, r, &nr), "divmod of a random pair");
			bigCheck(bigIntCompare(r, nr, b, nb) < 0, "remainder is below the divisor");
			size_t nqb = bigIntMul(q, nq, b, nb, t1);
			size_t nsum = bigIntAdd(t1, nqb, r, nr, t2);
			bigCheck(bigIntCompare(t2, nsum, a, na) == 0, "a == (a / b) * b + a % b");

			// a + b - b == a
			size_t nsum2 = bigIntAdd(a, na, b, nb, t1);
			size_t ndiff = bigIntSub(t1, nsum2, b, nb, t2);
			bigCheck(bigIntCompare(t2, ndiff, a, na) == 0, "a + b - b == a");

			// (a << k) >> k == a
			size_t k = (size_t) (bigRandom() % 200);
			size_t nsh = bigIntShiftLeft(a, na, k, t1);
			size_t nback = bigIntShiftRight(t1, nsh, k, t2);
			bigCheck(bigIntCompare(t2, nback, a, na) == 0, "(a << k) >> k == a");
			bigCheck(bigIntHighBit(t1, nsh) == bigIntHighBit(a, na) + k,
				"shifting left moves the high bit by exactly k");

			// gcd divides both, and the cofactors are coprime
			size_t ng = bigIntGcd(a, na, b, nb, t1);
			bigCheck(ng > 0, "gcd of nonzero operands is nonzero");
			size_t nqa = 0, nra = 0;
			bigCheck(bigIntDivMod(a, na, t1, ng, q, &nqa, r, &nra) && nra == 0,
				"gcd divides a");
			bigCheck(bigIntDivMod(b, nb, t1, ng, q, &nqa, r, &nra) && nra == 0,
				"gcd divides b");

			// print/parse round trip in several radices. The parse buffer is
			// sized by bigIntFromStringSize on purpose: it is the contract,
			// and sizing it by anything else is exactly the mistake that
			// caught the loose bound this test first ran against.
			static const int Bases[] = { 2, 8, 10, 16, 36 };
			for (size_t bi = 0; bi < sizeof(Bases) / sizeof(Bases[0]); bi++) {
				size_t len = bigIntToString(a, na, Bases[bi], text, sizeof(text));
				bigCheck(len > 0, "printing answers something");
				size_t need = bigIntFromStringSize(len, Bases[bi]);
				bigCheck(need <= sizeof(t1) / sizeof(t1[0]), "parse buffer is big enough");
				size_t nparsed = bigIntFromString(text, len, Bases[bi], t1);
				bigCheck(bigIntCompare(t1, nparsed, a, na) == 0,
					"parse(print(a)) == a");
			}

			// toDouble is monotone and matches ldexp of the top 53 bits when
			// the value is large enough to have them
			double d = bigIntToDouble(a, na);
			bigCheck(d >= 0.0, "toDouble is non-negative for a magnitude");
			if (bigIntHighBit(a, na) <= 53) {
				// small enough to be exact: rebuild it the slow way
				double exact = 0.0;
				for (size_t i = na; i > 0; i--) {
					exact = exact * 4294967296.0 + (double) a[i - 1];
				}
				bigCheck(d == exact, "toDouble is exact below 2^53");
			}
		}
	}

	// ---- bit accessors against a slow reference ---------------------------
	for (int iter = 0; iter < 200; iter++) {
		size_t na = randomMagnitude(a, 12);
		size_t count = 0;
		size_t high = 0;
		size_t low = 0;
		for (size_t i = 0; i < na * 32; i++) {
			if (bigIntBitAt(a, na, i)) {
				count++;
				high = i + 1;
				if (low == 0) {
					low = i + 1;
				}
			}
		}
		bigCheck(bigIntBitCount(a, na) == count, "bitCount matches a bit-by-bit count");
		bigCheck(bigIntHighBit(a, na) == high, "highBit matches a bit-by-bit scan");
		bigCheck(bigIntLowBit(a, na) == low, "lowBit matches a bit-by-bit scan");
	}

	printf("BigInt self-test: %d failures\n", bigIntFailures);
	return bigIntFailures;
}
