#include "jit/Specialize.h"
#include "jit/InlineCache.h"
#include "jit/Jit.h"
#include "runtime/Primitive.h"
#include "core/Assert.h"
#include "core/Class.h"
#include <stdlib.h>
#include <string.h>

// See jit/Specialize.h for what this file is and, more importantly, for what it
// refuses to assume.


// How dominant a site's class has to be before the optimizer speculates on it.
//
// NOT 100%, and not a knob to tune: a site that is 99% SmallInteger and 1%
// something else is exactly the site speculation is FOR. Guessing wrong there
// costs one deoptimization on the rare path; refusing to guess costs a full
// send on the common one. What 100% would exclude is every loop whose last
// iteration sees a different class, which is a large share of real code.
//
// The floor on EXECUTIONS matters more than the fraction and is separate: a
// site that ran twice has a 100% dominant class and no evidence behind it.
#define SPECIALIZE_DOMINANCE 0.90
#define SPECIALIZE_MIN_SENDS 8

double specializeDominanceThreshold(void) { return SPECIALIZE_DOMINANCE; }


// What one primitive number becomes, if anything.
//
// KEYED BY PRIMITIVE AND NOT BY SELECTOR. `+` is a name; PRIM_IntAdd is the
// method that packages/Core actually installed, found through the cache way
// that site resolved to. A program that redefines SmallInteger>>+ has a method
// with no primitive at all there, so it simply does not appear here and the
// send stays a send.
//
// The pairs that are NOT here are as deliberate as the ones that are:
//
//   PRIM_IntDiv has no entry because SmallInteger>>/ answers a FRACTION when
//   the division is not exact, and a Fraction allocates;
//   PRIM_IntFloorDiv, PRIM_IntMod, PRIM_IntQuo and PRIM_IntRem have none
//   because their answer for a zero or negative divisor is decided in Smalltalk;
//   PRIM_IntEquals does not exist -- SmallInteger>>= carries no primitive at
//   all, it is `==` after an isKindOf: -- so integer equality reaches this file
//   as an ordinary send and stays one.
typedef struct {
	uint16_t primitive;
	uint16_t op;
	uint16_t compare;
	_Bool checkOverflow;
} PrimitiveRule;

static const PrimitiveRule gRules[] = {
	// Integer arithmetic. Every one of the three can leave the 62-bit payload,
	// and the kernel's answer when it does is a LargeInteger built by the
	// method's own Smalltalk, which optimized code cannot build.
	{ PRIM_IntAdd, IR_IADD, 0, 1 },
	{ PRIM_IntSub, IR_ISUB, 0, 1 },
	{ PRIM_IntMul, IR_IMUL, 0, 1 },
	// Integer comparison. `<` is the only relational primitive the kernel
	// declares: Magnitude derives >, <= and >= from it in Smalltalk, so those
	// three arrive as sends to methods with no primitive and are not specialized
	// here. That is the correct answer and not a gap -- specializing `>` by name
	// would be specializing a method this site never called.
	{ PRIM_IntLessThan, IR_ICMP, IR_CMP_LT, 0 },
	// Float arithmetic. No overflow check: an IEEE double has no result that
	// fails to be a double, and infinity is an answer rather than a failure.
	{ PRIM_FloatAdd, IR_FADD, 0, 0 },
	{ PRIM_FloatSub, IR_FSUB, 0, 0 },
	{ PRIM_FloatMul, IR_FMUL, 0, 0 },
	{ PRIM_FloatDiv, IR_FDIV, 0, 0 },
	{ PRIM_FloatLessThan, IR_FCMP, IR_CMP_LT, 0 },
	{ PRIM_FloatEquals, IR_FCMP, IR_CMP_EQ, 0 },
};


static const PrimitiveRule *ruleFor(uint16_t primitive)
{
	if (primitive == PRIM_NONE) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(gRules) / sizeof(gRules[0]); i++) {
		if (gRules[i].primitive == primitive) {
			return &gRules[i];
		}
	}
	return NULL;
}


// Does the operation this rule names actually WORK on a value of `classIndex`?
//
// The primitive number says what the receiver's method does; it says nothing
// about the argument, and `3 + 4.0` reaches PRIM_IntAdd with a Float argument
// and answers a Float. Specializing that pair as an integer addition would
// unbox a SmallFloat64 as though it were a SmallInteger, which is a wrong
// answer and not a failed guard, so BOTH operands are checked against the
// representation the operation consumes and a mismatch is refused.
//
// Mixed arithmetic is therefore left as a send here. That is the first cut and
// it is stated as one: the pair is exactly the case an i2f plus a float
// operation would serve, and it needs the profile to be trusted about the
// ARGUMENT with the same weight it is trusted about the receiver.
static _Bool representationMatches(uint16_t op, uint32_t classIndex)
{
	_Bool wantsFloat = op == IR_FADD || op == IR_FSUB || op == IR_FMUL
		|| op == IR_FDIV || op == IR_FCMP;
	// The immediate classes, from the same table the guard the optimizer emits
	// will compare against (core/Class.h). Asking the table rather than the
	// Handles keeps this answer and the guard's answer the same one.
	uint32_t smallInteger = gClassIndexByTag[VALUE_INT];
	uint32_t smallFloat = gClassIndexByTag[VALUE_FLOAT];
	return classIndex == (wantsFloat ? smallFloat : smallInteger);
}


SiteSpecialization *specializeFor(NativeCode *code, uint32_t *specialized)
{
	if (specialized != NULL) {
		*specialized = 0;
	}
	if (code == NULL || code->cells == NULL || code->unit == NULL
			|| code->unit->instructionCount == 0) {
		return NULL;
	}

	uint16_t count = code->unit->instructionCount;
	SiteSpecialization *sites = calloc(count, sizeof(SiteSpecialization));
	ASSERT(sites != NULL);
	uint32_t decided = 0;
	for (uint16_t bci = 0; bci < count; bci++) {
		// Every entry starts as "do not specialize", which is what a zeroed op
		// would NOT say: IR op zero is IR_CONST. Written explicitly for every
		// site rather than relying on calloc, because the default here has to be
		// the refusal and a zero that means something else is how it would stop
		// being.
		sites[bci].op = IR_OP_COUNT;

		const IcCell *cell = &code->cells[bci];
		if (cell->selector == NULL || cell->sends < SPECIALIZE_MIN_SENDS) {
			continue;
		}
		double fraction = 0.0;
		uint32_t receiverClass = icDominantClass(cell, &fraction);
		if (receiverClass == CLASS_INDEX_INVALID
				|| fraction < SPECIALIZE_DOMINANCE) {
			continue;
		}
		const IcWay *way = icDominantWay(cell);
		// THE METHOD THIS SITE RESOLVED TO, which is the whole check. A way with
		// no target is a cache the runtime cleared after a method dictionary
		// changed (jitFlushSendCaches), and a site whose resolution was just
		// invalidated is precisely the one not to speculate about.
		if (way == NULL || way->target == NULL || way->target->unit == NULL) {
			continue;
		}
		const PrimitiveRule *rule = ruleFor(way->target->unit->primitive);
		if (rule == NULL) {
			continue;
		}
		// A binary operation, so the method takes exactly one argument, and the
		// profile has to have seen what that argument was.
		uint32_t argumentClass = way->argClassIndex;
		if (way->target->unit->argumentCount != 1
				|| argumentClass == CLASS_INDEX_INVALID) {
			continue;
		}
		if (!representationMatches(rule->op, receiverClass)
				|| !representationMatches(rule->op, argumentClass)) {
			continue;
		}

		sites[bci].op = rule->op;
		sites[bci].compare = rule->compare;
		sites[bci].receiverClass = receiverClass;
		sites[bci].argumentClass = argumentClass;
		sites[bci].checkOverflow = rule->checkOverflow;
		decided++;
	}

	if (specialized != NULL) {
		*specialized = decided;
	}
	if (decided == 0) {
		// Nothing to say, and saying nothing is cheaper than saying it in an
		// array the optimizer then walks for every send in the method.
		free(sites);
		return NULL;
	}
	return sites;
}
