// Running the FRONT HALF of tier 2 over real code, and reporting what it can
// consume.
//
// Separate translation unit, and that is the point rather than tidiness: gate
// levels 3, 7 and 8 hand-link jit/Jit.c with half a dozen files and no optimizer
// at all, so the reference to SsaBuild has to live somewhere those levels do not
// link. jit/Jit.c carries a WEAK no-op of the entry point below and this file
// overrides it, exactly the seam memory/Roots.h uses for the same problem.

#include "jit/Jit.h"
#include "jit/SsaBuild.h"
#include "jit/Passes.h"
#include "jit/Deopt.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>

// ST_TIER2_DRYRUN=1: build SSA and run the optimizer over every method the VM
// compiles, check the deopt states it produced, and THROW THE RESULT AWAY.
//
// It answers the question that sizes what is left of tier 2. The front half of
// it -- SsaBuild, the passes, the deopt states -- has only ever run on bytecode
// written by hand in gate levels 4, 5 and 6. `packages/Core` is 1727 methods of
// real code, so running the front half over all of them says how much of the
// kernel the IR can consume TODAY and names the opcode for each method it
// cannot. That list is the work, instead of an estimate of the work.
//
// Nothing observable changes: the template compiler's output is what runs, and
// the IR is destroyed before this returns.
typedef struct {
	uint64_t attempted;
	uint64_t built;
	uint64_t refused;
	uint64_t optimized;
	uint64_t malformedStates;
	uint64_t refusedByOpcode[OP_COUNT];
} Tier2DryRun;

static Tier2DryRun gTier2DryRun;

static void tier2DryRunReport(void)
{
	fprintf(stderr, "tier2: attempted=%llu built=%llu refused=%llu"
		" optimized=%llu malformedStates=%llu\n",
		(unsigned long long) gTier2DryRun.attempted,
		(unsigned long long) gTier2DryRun.built,
		(unsigned long long) gTier2DryRun.refused,
		(unsigned long long) gTier2DryRun.optimized,
		(unsigned long long) gTier2DryRun.malformedStates);
	for (int op = 0; op < OP_COUNT; op++) {
		if (gTier2DryRun.refusedByOpcode[op] != 0) {
			fprintf(stderr, "tier2:   refused by %-10s %llu\n",
				opcodeName((Opcode) op),
				(unsigned long long) gTier2DryRun.refusedByOpcode[op]);
		}
	}
	fflush(NULL);
}


static _Bool tier2DryRunEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_TIER2_DRYRUN") != NULL;
		if (enabled) {
			atexit(tier2DryRunReport);
		}
	}
	return enabled != 0;
}


// Every deopt state the optimized function still carries has to name values that
// survived. A state that mentions a deleted value is the defect Deopt.h says can
// only be caught here: at run time it is a wrong answer on the path nobody takes.
static void tier2CheckDeoptStates(IrFunction *function)
{
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			if (value->deopt != NULL
					&& !deoptStateIsWellFormed(value->deopt)) {
				gTier2DryRun.malformedStates++;
			}
		}
	}
}


void tier2DryRun(CodeUnit *unit)
{
	if (!tier2DryRunEnabled()) {
		return;
	}
	gTier2DryRun.attempted++;
	Opcode unsupported = OP_COUNT;
	IrFunction *function = ssaBuild(unit, &unsupported);
	if (function == NULL) {
		gTier2DryRun.refused++;
		if ((int) unsupported < OP_COUNT) {
			gTier2DryRun.refusedByOpcode[unsupported]++;
		}
		return;
	}
	gTier2DryRun.built++;
	irOptimize(function);
	gTier2DryRun.optimized++;
	tier2CheckDeoptStates(function);
	irDestroy(function);
}


