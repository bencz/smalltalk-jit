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
// ST_TYPE_STATS=1  send-classification census + exit dump (TypeStats below).

#include "core/CompiledCode.h"
#include "compiler/Bytecodes.h"
#include <stdlib.h>
#include <stdint.h>

// Once-only latch in NativeCode.tags, set under codegenLock by the recompile
// trigger. Exec-space code is never swept or moved, and its walkers read only
// TAG_FREESPACE, so this bit is free (Object.h uses 1, 4, 8, 16, 32).
#define TIER_TAG_FIRED (1 << 6)

typedef struct {
	size_t countedMethods;      // framed tier-0 methods given a counter + check
	size_t filteredMethods;     // framed methods with no dynamic send, check elided
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


// Send-classification census (ST_TYPE_STATS=1): how many sends this VM already
// resolves without dispatch, and for the ones it does not, WHAT THE RECEIVER IS.
// This is the measurement gate for optional type annotations: the whole thesis
// is that declaring a parameter's class would let
// compiledCodeResolveOperandClass answer for OPERAND_ARG_VAR (and
// OPERAND_TEMP_VAR), at which point the backends' existing compile-time
// devirtualization fires with no other change. The census says how much code
// that would actually reach, before any syntax exists.
//
// TWO SCOPES, because they answer different questions:
//
//   ALL  every framed method the tier-0 codegen sees, counted once each. The
//        shape of the source as WRITTEN, kernel included. A method compiled but
//        never run counts the same as the hottest loop in the program.
//   HOT  the same census over methods that reached TIER 1, counted in
//        compiler/Optimizer.c. Reaching tier 1 IS the hotness filter, so this
//        is the decision-relevant number; ALL is its denominator and sanity
//        check. Use a low ST_TIER_THRESHOLD to widen the HOT sample.
//
// Neither scope weights a site by how many times it EXECUTES: a site in a loop
// body counts once. Reading these as "fraction of dispatches" would overstate
// cold code, so they are reported as what they are, a census of SITES.
//
// The refinement that decides the thesis, over HOT arg/temp receivers only:
// what a declared class would actually BUY at that site. A site whose IC cell
// is MONO has ALREADY been devirtualized by the tier (guarded direct call, or
// inlined body), so an annotation buys the removal of a 2-to-5 instruction
// guard, not the removal of a dispatch. A site whose cell is a PIC genuinely
// sees several classes, so an EXACT declared type there would be a lie that
// raises. Only the cold and mega buckets are places where a type declaration
// tells the compiler something the runtime never learned.
typedef struct {
	// scope ALL
	size_t methods;
	size_t sendsStatic;      // receiver class resolvable today: no cell, no dispatch
	size_t sendsIdentity;    // ==/~~/isNil/notNil: compiled inline, no cell
	size_t sendsDynamic;
	size_t dynSelf;          // OPERAND_TEMP_VAR at SELF_INDEX: annotation cannot help
	size_t dynArg;           // OPERAND_ARG_VAR: the thesis target
	size_t dynTemp;          // OPERAND_TEMP_VAR other than self: the thesis target
	size_t dynInstVar;       // OPERAND_INST_VAR: typed-ivar territory, not M12
	size_t dynContextVar;    // OPERAND_CONTEXT_VAR: captured, needs closure typing
	size_t dynOther;         // ASSOC (globals), INST_VAR_OF, anything else
	// scope HOT (tier-1 methods only)
	size_t hotMethods;
	size_t hotStatic;
	size_t hotIdentity;
	size_t hotDynamic;
	size_t hotSelf;
	size_t hotArg;
	size_t hotTemp;
	size_t hotInstVar;
	size_t hotContextVar;
	size_t hotOther;
	// what an exact declared type would buy, over hotArg + hotTemp only
	size_t annotMono;        // already devirtualized: buys a guard, not a dispatch
	size_t annotPic;         // several classes seen: an exact type would be WRONG
	size_t annotMega;        // megamorphic: a type would be new information
	size_t annotColdNever;   // unlinked and never executed: a type IS new information
	size_t annotColdReset;   // unlinked but executed earlier: the GC wiped what was known
} TypeStats;
extern TypeStats gTypeStats;

void typePrintStats(void);

// An unlinked cell at recompile time means one of two very different things,
// and telling them apart is what makes the census decisive. A site that NEVER
// RAN is a place where a declared type tells the compiler something the runtime
// could not know. A site that ran and whose cell was then wiped by the STW
// reset (icResetNativeCodeCells runs at every scavenge, so a long run resets
// each cell many times over) is a place where the runtime ALREADY KNEW and the
// collector threw it away: there, persistent type feedback is the cheap fix and
// a type system is the expensive one.
//
// Reaching inlineCacheMiss at all proves the site executed, so recording the
// cell there is an exact witness. Stats-only and gated: zero cost, and no more
// thread-safe than the plain gIcStats increments next to it (measurement
// tooling, single worker).
void typeStatsNoteExecuted(struct IcCell *cell);

// One census entry. `resolved` and `identity` are the caller's own
// classification (jit/SendClassify.h), passed in rather than recomputed so the
// census can never disagree with the decision the compiler actually made.
// `cell` is the site's IC feedback and is meaningful only when hot; NULL
// elsewhere. Costs nothing unless ST_TYPE_STATS is set.
void typeStatsNoteSend(_Bool hot, Operand receiver, _Bool identity, _Bool resolved,
	struct IcCell *cell);

// The JIT prologue's slow path: `insts` is the code entry (NativeCode.insts)
// of the method whose invocation counter just hit zero.
void tierRecompile(uint8_t *insts);
void tierPrintStats(void);

// Whether a tier-0 framed method is worth a counter + prologue check: true iff
// it has at least one dynamic send (an IC-cell site the recompile could
// promote or inline). Methods with none are frozen at tier 0.
_Bool tierMethodHasDynamicSend(CompiledCode *code);

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


static _Bool typeStatsEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_TYPE_STATS") != NULL;
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
//
// The default was 24 while callees had to be straight-line. Once the inliner
// learned control flow, 24 turned out to admit NOTHING new: an ifTrue:ifFalse:
// alone is a guard plus two jumps plus both arms, comfortably past it. Measured
// inlinedSites at each ceiling, straight-line-only versus with control flow:
//
//   ceiling   Richards        DeltaBlue
//   24        17 -> 17        73 -> 73     control flow admits nothing
//   48        17 -> 17        89 -> 89     still nothing; the growth is the cap
//   96        17 -> 19        92 -> 103
//   256       17 -> 24        93 -> 108
//
// So the size ceiling, not the straight-line rule, was the real gate. At 96 the
// executed-instruction counts fell 0.79% on Richards and 1.40% on DeltaBlue,
// while MixedArithBench, FloatBench and ArrayNumericBench moved by 0.000%.
//
// The ceiling then sat at 96 for one reason only: a callee branching on its own
// instance variable was REJECTED, because adjustOperand rewrites the ivar into
// OPERAND_INST_VAR_OF and generateClassCheck had no arm for that form, so 128
// crashed Richards. With both backends teaching generateClassCheck the form,
// re-measured:
//
//   ceiling   Richards   DeltaBlue
//   96        19         103        (unchanged: the arm alone admits nothing here)
//   128       21         104
//   192       25         105
//   256       25         108
//
// 192 is the default now. Note what the arm is and is not worth: at 96 it
// unlocks ZERO sites across the whole test corpus, so it only pays TOGETHER
// with the higher ceiling, where it contributes 1 of Richards' 6 new sites. 256
// buys DeltaBlue 3 more and Richards nothing, which is not worth the extra code
// growth on a layout lottery this VM has lost before (see the tier slow-path
// out-of-lining note below).
static size_t tierInlineMax(void)
{
	static long limit = -1;
	if (limit < 0) {
		char *env = getenv("ST_TIER_INLINE_MAX");
		limit = env != NULL ? atol(env) : 192;
		if (limit < 0) {
			limit = 0;
		}
	}
	return (size_t) limit;
}


// Marks a tierSiteMap slot whose instruction is the SPECULATIVE INLINE GUARD
// the optimizer emitted, not a send. Both the compiler (inlined boolean
// control flow, guarding True/False) and the optimizer emit
// BYTECODE_JUMP_NOT_MEMBER_OF, and the backends cannot tell them apart from
// the bytecode alone: only the optimizer's is a speculation that a class
// redefinition must be able to poison (jit/SpecSite.h).
//
// The map is indexed by instruction number and mapSet only ever writes real
// cells for SENDS, so a jump's slot is otherwise unused and free to carry this
// sentinel. Its ADDRESS is the marker; the object is never dereferenced.
extern IcCell gTierSpecGuard;

static inline _Bool tierSiteIsSpecGuard(IcCell **siteMap, size_t siteMapSize, ptrdiff_t instruction)
{
	return siteMap != NULL && instruction >= 0 && (size_t) instruction < siteMapSize
		&& siteMap[instruction] == &gTierSpecGuard;
}


#endif
