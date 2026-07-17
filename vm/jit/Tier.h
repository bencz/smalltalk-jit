#ifndef TIER_H
#define TIER_H

// Adaptive recompilation (tier 1), fed by the per-site inline caches
// (jit/InlineCache.h). Every framed METHOD is born tier 0 with an invocation
// counter in NativeCode.counter (initialized to the threshold; the emitted
// prologue check DECREMENTS it and fires exactly when it reaches zero). The
// one-time recompile re-runs the normal code generator over the SAME bytecodes
// with `CodeGenerator.tierFeedback` pointing at the superseded NativeCode:
// each dynamic send whose old IC cell is bound (mono, or a pic's way 0) is
// promoted to an exact-class guard plus a direct call to the baked entry, with
// the ordinary IC send as the fallback. There is NO deoptimization and NO
// on-stack replacement: every speculation keeps today's send as its floor, and
// republishing over compiledMethodSetNativeCode is safe because exec space
// never moves or frees (in-flight frames finish in the old code, which stays
// valid forever).
//
// Known, accepted staleness: a caller that BAKED a static entry (or an IC cell
// bound before the republish) keeps calling the superseded code until its own
// recompile or the next STW cell reset rebinds it. Frameless methods have no
// prologue and stay tier 0 (they contain no sends, so there is nothing to
// promote in them anyway); blocks carry no counter and are re-tiered together
// with their method (a block's NativeCode is baked into the method's code, so
// recompiling the block alone would never be called).
//
// ST_NO_TIER=1     kill-switch: no counter, no trigger, exactly tier-0 code.
// ST_TIER_THRESHOLD=<n>  invocations before the recompile (default 1000).
// ST_TIER_STATS=1  counters + exit dump; the JIT-emitted increments (guard
//                  hits/fails) cost only under this flag.

#include "core/CompiledCode.h"
#include <stdlib.h>
#include <stdint.h>

// Once-only latch in NativeCode.tags, set under codegenLock by the recompile
// trigger. Exec-space code is never swept or moved, and its walkers read only
// TAG_FREESPACE, so this bit is free (Object.h uses 1, 4, 8, 16, 32).
#define TIER_TAG_FIRED (1 << 6)

typedef struct {
	size_t triggerCalls;        // C trigger invocations (races echo; idempotent)
	size_t recompiles;          // tier-1 recompilations published, once per method
	size_t discardedRecompiles; // recompiles thrown away: zero sites promoted
	size_t promotedSites;       // dynamic sends promoted to guarded direct calls
	size_t unpromotedSites;     // tier-1 sends left as plain IC (no usable feedback)
	size_t inlinedSites;        // dynamic sends replaced by a guarded inlined body
	size_t directCalls;         // guard hits, JIT-emitted only under ST_TIER_STATS
	size_t guardFails;          // guard misses -> IC fallback, same gating
} TierStats;
extern TierStats gTierStats;

// The JIT prologue's slow path: `insts` is the code entry (NativeCode.insts)
// of the method whose invocation counter just hit zero.
void tierRecompile(uint8_t *insts);
void tierPrintStats(void);

// One malloc'd countdown word per tier-0 framed method, allocated at emission
// and baked into the prologue check as a plain immediate. The counter is
// deliberately NOT stored in the NativeCode header: that word (insts-8)
// shares a cache line with the method's first instructions, and on x86 a
// store into a line holding in-flight instructions trips the self-modifying
// -code detector for a full pipeline clear PER INVOCATION (measured 4x on
// Richards). A C-heap word is on an unrelated, never-executed page. The cell
// leaks by design, exactly like the exec-space code it belongs to.
size_t *tierAllocCounter(void);


static _Bool tierEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		// The IC cells ARE the type feedback: without them (ST_NO_IC) a
		// recompile could promote nothing, so the whole tier stays off.
		enabled = getenv("ST_NO_TIER") == NULL && icEnabled();
	}
	return enabled;
}


static _Bool tierStatsEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_TIER_STATS") != NULL;
	}
	return enabled;
}


static size_t tierThreshold(void)
{
	static size_t threshold = 0;
	if (threshold == 0) {
		char *env = getenv("ST_TIER_THRESHOLD");
		long value = env != NULL ? atol(env) : 0;
		threshold = value > 0 ? (size_t) value : 1000;
	}
	return threshold;
}


// Byte-size ceiling for a callee's bytecode stream to qualify for speculative
// inlining (compiler/Optimizer.c). ST_TIER_INLINE_MAX=0 disables inlining
// while keeping the rest of the tier (the isolation knob for A/B).
static size_t tierInlineMax(void)
{
	static long limit = -1;
	if (limit < 0) {
		char *env = getenv("ST_TIER_INLINE_MAX");
		limit = env != NULL ? atol(env) : 24;
		if (limit < 0) {
			limit = 0;
		}
	}
	return (size_t) limit;
}


#endif
