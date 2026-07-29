#ifndef PASSES_H
#define PASSES_H

// The optimizer. See Passes.c for why the ORDER matters more than the passes.

#include "jit/Ir.h"

// What the optimizer may substitute for ONE send site, decided from that site's
// type profile BEFORE the optimizer runs and handed in as DATA.
//
// DATA AND NOT A REACH INTO THE INLINE CACHE, and the split is not tidiness.
// Gate level 5 links Passes.c with the IR and nothing else, so a pass that read
// an IcCell could not be proved there at all; and the whole of "IntAddPrimitive
// means IR_IADD" ends up in ONE file (jit/Specialize.c), which is the half that
// has to keep agreeing with packages/Core. Here the decision is already made.
//
// The array is indexed by BYTECODE INDEX, the same coordinate the cache cells,
// the deopt states and the machine-offset map all already use.
typedef struct {
	// The IR operation to compute with, or IR_OP_COUNT for "do not specialize",
	// which is what every site that has not proved otherwise holds.
	uint16_t op;
	// The comparison kind for IR_ICMP and IR_FCMP, in the encoding those two
	// carry in `extra`. Ignored for the arithmetic operations.
	uint16_t compare;
	// The classes the receiver and the argument are guarded to. BOTH are
	// guarded: `a + b` is specialized on what both operands are, and a profile
	// that saw only the receiver is a profile that cannot justify the argument.
	uint32_t receiverClass;
	uint32_t argumentClass;
	// May the result leave the SmallInteger range? See IR_FLAG_CHECK_OVERFLOW.
	_Bool checkOverflow;
} SiteSpecialization;

typedef struct {
	uint32_t trivialPhis;
	uint32_t typesLearned;
	uint32_t guardsRemoved;
	uint32_t simplified;    // box/unbox pairs and field-of-new reads
	uint32_t gvnRemoved;
	uint32_t scalarReplaced;
	uint32_t materializations;
	uint32_t hoisted;
	uint32_t phisPromoted;
	uint32_t deadRemoved;
	uint32_t blocksMerged;
	uint32_t allocationsRemaining;
	// Sends that became raw arithmetic behind guards, and sends that a profile
	// offered and the IR could not take. The second is not noise: a site the
	// bridge decided on and the pass then declined is a disagreement between
	// two halves, which is the defect shape this VM keeps paying for.
	uint32_t sendsSpecialized;
	uint32_t specializationsDeclined;
	// Boxes whose only remaining reader was a deoptimization state, which can
	// name the raw value instead and re-box on the way out.
	uint32_t boxesSunk;
} PassStats;

// Everything the optimizer is told about the RUNNING system, which is a small
// and deliberately closed set: what each send site has been seen to do, and the
// one global fact below.
typedef struct {
	const SiteSpecialization *sites;   // indexed by bytecode index
	uint16_t siteCount;
	// The class index of a tagged SmallInteger.
	//
	// A GLOBAL FACT rather than a per-site one, and it earns its place by what
	// it removes: without it a literal `1` has no known class, so `x + 1` guards
	// the constant at run time and an accumulator seeded with `0` has a phi
	// whose operands do not agree on a class -- which blocks the guard removal,
	// which leaves a box on the loop-carried edge, which is the exact cost phi
	// promotion exists to remove. One index recovers all of it.
	//
	// It is passed IN rather than read here because gate level 5 links this file
	// with the IR and nothing else: there is no class table there, and there
	// should not be. CLASS_INDEX_INVALID means "not known", which is what every
	// caller without a running heap passes.
	uint32_t smallIntegerClass;
} IrProfile;

// `profile` may be NULL, which is the whole of "nothing is known": every send
// stays a send and no constant has a class. That is what gate levels 4 to 6 and
// the dry run over freshly compiled methods all want.
PassStats irOptimize(IrFunction *function, const IrProfile *profile);

#endif
