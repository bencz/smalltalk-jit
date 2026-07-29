// Running the FRONT HALF of tier 2 over real code, and reporting what it can
// consume.
//
// Separate translation unit, and that is the point rather than tidiness: gate
// levels 3, 8 and 9 hand-link jit/Jit.c with half a dozen files and no optimizer
// at all, so the reference to SsaBuild has to live somewhere those levels do not
// link. jit/Jit.c carries a WEAK no-op of the entry point below and this file
// overrides it, exactly the seam memory/Roots.h uses for the same problem.

#include "jit/Jit.h"
#include "jit/SsaBuild.h"
#include "jit/Passes.h"
#include "jit/Deopt.h"
#include "jit/Lower.h"
#include "jit/RegAlloc.h"
#include "jit/MacroAssembler.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// How many distinct IR ops the BACKEND can refuse over. Names rather than a
// count, because "16 methods refused" is not a work list and "16 refused by
// guard_class" is.
#define REFUSAL_NAMES 16

typedef struct {
	uint64_t attempted;
	uint64_t built;
	uint64_t refused;
	uint64_t optimized;
	uint64_t malformedStates;
	uint64_t refusedByOpcode[OP_COUNT];
	// The BACK half: how far past the optimizer each method gets.
	uint64_t lowered;
	uint64_t lowerRefused;
	uint64_t allocated;
	uint64_t allocFailed;
	uint64_t verifyFailed;
	uint64_t spilled;
	uint64_t splits;
	uint64_t frameMaps;
	uint64_t deoptSites;
	uint64_t deoptIncomplete;
	const char *refusedName[REFUSAL_NAMES];
	uint64_t refusedNameCount[REFUSAL_NAMES];
} Tier2DryRun;

static Tier2DryRun gTier2DryRun;


// Refusals are counted BY NAME, in a small fixed table. The names are string
// literals out of irOpName, so they compare by pointer and the table needs no
// allocation; overflowing it would silently lose a category, so it says so.
static void countRefusal(const char *name)
{
	for (int i = 0; i < REFUSAL_NAMES; i++) {
		if (gTier2DryRun.refusedName[i] == NULL) {
			gTier2DryRun.refusedName[i] = name;
			gTier2DryRun.refusedNameCount[i] = 1;
			return;
		}
		if (strcmp(gTier2DryRun.refusedName[i], name) == 0) {
			gTier2DryRun.refusedNameCount[i]++;
			return;
		}
	}
	fprintf(stderr, "tier2: more than %d refusal categories, dropping %s\n",
		REFUSAL_NAMES, name);
}


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
	fprintf(stderr, "tier2: lowered=%llu lowerRefused=%llu allocated=%llu"
		" allocFailed=%llu verifyFailed=%llu\n",
		(unsigned long long) gTier2DryRun.lowered,
		(unsigned long long) gTier2DryRun.lowerRefused,
		(unsigned long long) gTier2DryRun.allocated,
		(unsigned long long) gTier2DryRun.allocFailed,
		(unsigned long long) gTier2DryRun.verifyFailed);
	fprintf(stderr, "tier2: spilled=%llu splits=%llu frameMaps=%llu\n",
		(unsigned long long) gTier2DryRun.spilled,
		(unsigned long long) gTier2DryRun.splits,
		(unsigned long long) gTier2DryRun.frameMaps);
	fprintf(stderr, "tier2: deoptSites=%llu deoptIncomplete=%llu\n",
		(unsigned long long) gTier2DryRun.deoptSites,
		(unsigned long long) gTier2DryRun.deoptIncomplete);
	for (int i = 0; i < REFUSAL_NAMES && gTier2DryRun.refusedName[i] != NULL; i++) {
		fprintf(stderr, "tier2:   lowering refused by %-12s %llu\n",
			gTier2DryRun.refusedName[i],
			(unsigned long long) gTier2DryRun.refusedNameCount[i]);
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

	// THE BACK HALF, over the same real code. The lowering and the allocator had
	// only ever run on hand-written input; `packages/Core` is 1727 methods, so
	// this says how much of the kernel the backend can consume TODAY and names
	// the operation behind every refusal. That list is the work, instead of an
	// estimate of the work -- the same reason the front half is measured here.
	//
	// NO tier-1 NativeCode is passed, so send sites lower with a null cache
	// cell. That is correct for a dry run and would not be for emission: the
	// cell is what a send site reads, and tier 2 shares tier 1's rather than
	// growing a second set. Nothing is emitted here.
	const char *refused = NULL;
	LirFunction *lir = lirLower(function, maHostBackend()->abi, NULL, &refused);
	if (lir == NULL) {
		gTier2DryRun.lowerRefused++;
		countRefusal(refused != NULL ? refused : "?");
		irDestroy(function);
		return;
	}
	gTier2DryRun.lowered++;

	RegAllocStats allocation = lirAllocateRegisters(lir);
	if (allocation.failed) {
		gTier2DryRun.allocFailed++;
	} else {
		gTier2DryRun.allocated++;
		gTier2DryRun.spilled += allocation.spilled;
		gTier2DryRun.splits += allocation.splits;
		gTier2DryRun.frameMaps += allocation.frameMaps;
		gTier2DryRun.deoptSites += allocation.deoptSites;
		gTier2DryRun.deoptIncomplete += allocation.deoptIncomplete;
		// The verifier runs on EVERY method here, which is the whole point of
		// having one: two live values in one register produces a wrong answer
		// somewhere else entirely, and this is the only place that sees the
		// allocation itself.
		if (!lirVerifyAllocation(lir)) {
			gTier2DryRun.verifyFailed++;
		}
	}

	lirDestroy(lir);
	irDestroy(function);
}


