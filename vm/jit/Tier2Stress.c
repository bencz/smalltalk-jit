// Making tier 2 RUN, so that --deopt-stress measures something.
//
// ---------------------------------------------------------------------------
// THIS IS A TESTING MODE AND NOT THE TIER POLICY
// ---------------------------------------------------------------------------
//
// The tier policy -- when a method is hot enough, what invalidates optimized
// code when a method dictionary changes, how a running loop transfers into
// optimized code -- is a separate piece of work with its own decisions. None of
// it is here. What is here is the crudest possible driver: EVERY method tier 2
// can compile is compiled and installed, at the first send that reaches it.
//
// It exists because the alternative is a dishonest green. `ST_DEOPT_STRESS=1`
// over a system where no optimized code ever executes reports that every test
// still passes, which is true and means nothing: there was nothing to leave.
// A stress mode has to be able to fail before its passing is evidence.
//
// ---------------------------------------------------------------------------
// WHY THE UPGRADE HAPPENS AT THE FIRST SEND AND NOT WHEN THE METHOD IS HOT
// ---------------------------------------------------------------------------
//
// Because of where a send site keeps its answer. The emitted fast path reads
// `way->target` and jumps to that NativeCode's entry, WITHOUT consulting
// `method->native` (jit/x64/MacroAssemblerX64.c). So a site armed while the
// method was still tier 1 keeps entering tier 1 forever, and replacing
// `method->native` later is invisible to it.
//
// Upgrading on the first dispatch avoids that entirely: no site has armed to
// this method yet, and the site that is missing right now arms to whatever
// `method->native` holds a few lines further down. Every later site misses once
// and arms to the same thing. Coverage is total for free.
//
// The cost is that the profile is EMPTY at that moment, so nothing specializes
// and the optimized code carries no guard of its own. That is why the stress
// mode ADDS a guard at every send site rather than relying on the profile to
// have produced one (jit/Passes.c). The two halves fit: this driver gets
// optimized code executing everywhere, and the stress guards make all of it
// leave.
//
// SEPARATE TRANSLATION UNIT, and for the reason jit/Tier2DryRun.c gives: gate
// levels 3, 8 and 9 hand-link jit/Jit.c with half a dozen files and no tier 2,
// so the reference from Jit.c is a weak symbol this file overrides.

#include "jit/Jit.h"
#include "jit/SsaBackend.h"
#include "jit/SsaEmitter.h"
#include "jit/Deopt.h"
#include "jit/MacroAssembler.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>


typedef struct {
	uint64_t considered;
	uint64_t upgraded;
	uint64_t refused;
	const char *lastRefusal;
} Tier2StressStats;

static Tier2StressStats gStats;


static void tier2StressReport(void)
{
	fprintf(stderr, "tier2: stress considered=%llu upgraded=%llu refused=%llu",
		(unsigned long long) gStats.considered,
		(unsigned long long) gStats.upgraded,
		(unsigned long long) gStats.refused);
	if (gStats.lastRefusal != NULL) {
		fprintf(stderr, " (last: %s)", gStats.lastRefusal);
	}
	// AND HOW MANY TIMES OPTIMIZED CODE WAS ACTUALLY LEFT, which is the number
	// that keeps --deopt-stress from being a green that means nothing: a stress
	// run whose count is zero did not stress anything, whatever its tests said.
	fprintf(stderr, " left=%llu\n",
		(unsigned long long) jitDeoptimizationCount());
	fflush(NULL);
}


// ST_TIER2_ALL=1 runs tier 2 with its speculations intact, which is the pure
// equivalence question: does the second code generator answer what the first
// one does, over the whole system. ST_DEOPT_STRESS=1 implies it and additionally
// makes every speculation fail. Both need this driver, so both turn it on.
static _Bool tier2StressEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_TIER2_ALL") != NULL || deoptStressEnabled();
		if (enabled) {
			atexit(tier2StressReport);
		}
	}
	return enabled != 0;
}


NativeCode *tier2StressUpgrade(NativeCode *tier1)
{
	if (tier1 == NULL || tier1->optimized || tier1->tier2Attempted
			|| !tier2StressEnabled()) {
		return NULL;
	}
	// ONCE PER METHOD, whatever the outcome. A refusal is a property of the
	// method and of this backend, so retrying it on every send would pay the
	// whole front half of tier 2 again for the same answer.
	tier1->tier2Attempted = 1;
	gStats.considered++;

	const char *refused = NULL;
	NativeCode *code = ssaCompile(ssaHostBackend(), tier1->unit, tier1, &refused,
		NULL);
	if (code == NULL) {
		gStats.refused++;
		gStats.lastRefusal = refused;
		return NULL;
	}
	// The WIDE convention has to agree, or a caller hands the callee a pointer
	// where it reads registers. Both tiers derive it from the same arity against
	// the same ABI, so it cannot differ; it is checked because the failure is a
	// callee reading arguments nobody wrote, which is a wrong answer and not a
	// crash.
	ASSERT(code->wide == tier1->wide);
	code->optimized = 1;
	gStats.upgraded++;
	return code;
}
