#include "jit/Tier.h"
#include "jit/CodeGenerator.h"
#include "jit/SendClassify.h"
#include "jit/InlineCache.h"
#include "compiler/Optimizer.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecodes.h"
#include "core/CompiledCode.h"
#include "core/Lookup.h"
#include "core/Thread.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "memory/Heap.h"
#include "core/Assert.h"
#include <stdio.h>

TierStats gTierStats = { 0 };
TypeStats gTypeStats = { 0 };

// Address-only sentinel for tierSiteMap slots holding a speculative inline
// guard rather than a send's feedback cell (see tierSiteIsSpecGuard). Never
// dereferenced, never published into emitted code.
IcCell gTierSpecGuard = { NULL };


size_t *tierAllocCounter(void)
{
	size_t *cell = malloc(sizeof(size_t));
	*cell = tierThreshold();
	return cell;
}


// Open-addressed set of cell addresses that reached the IC miss handler, i.e.
// that provably executed. Power-of-two capacity, linear probing, never deleted
// from, doubled at half full. Only ever populated under ST_TYPE_STATS.
static IcCell **gExecutedSites = NULL;
static size_t gExecutedCap = 0;
static size_t gExecutedCount = 0;


static size_t executedSlot(IcCell **table, size_t cap, IcCell *cell)
{
	// The low bits of a malloc'd cell address are structurally zero, so mix
	// before masking or every probe collides in the same few slots.
	size_t hash = (size_t) (uintptr_t) cell;
	hash = (hash >> 4) ^ (hash >> 20);
	size_t mask = cap - 1;
	size_t i = hash & mask;
	while (table[i] != NULL && table[i] != cell) {
		i = (i + 1) & mask;
	}
	return i;
}


void typeStatsNoteExecuted(IcCell *cell)
{
	if (!typeStatsEnabled() || cell == NULL) {
		return;
	}
	if (gExecutedCount * 2 >= gExecutedCap) {
		size_t cap = gExecutedCap == 0 ? 1024 : gExecutedCap * 2;
		IcCell **table = calloc(cap, sizeof(IcCell *));
		for (size_t i = 0; i < gExecutedCap; i++) {
			if (gExecutedSites[i] != NULL) {
				table[executedSlot(table, cap, gExecutedSites[i])] = gExecutedSites[i];
			}
		}
		free(gExecutedSites);
		gExecutedSites = table;
		gExecutedCap = cap;
	}
	size_t i = executedSlot(gExecutedSites, gExecutedCap, cell);
	if (gExecutedSites[i] == NULL) {
		gExecutedSites[i] = cell;
		gExecutedCount++;
	}
}


static _Bool typeStatsHasExecuted(IcCell *cell)
{
	if (gExecutedCap == 0) {
		return 0;
	}
	return gExecutedSites[executedSlot(gExecutedSites, gExecutedCap, cell)] == cell;
}


void typeStatsNoteSend(_Bool hot, Operand receiver, _Bool identity, _Bool resolved,
	IcCell *cell)
{
	if (!typeStatsEnabled()) {
		return;
	}
	size_t *staticSlot = hot ? &gTypeStats.hotStatic : &gTypeStats.sendsStatic;
	size_t *identitySlot = hot ? &gTypeStats.hotIdentity : &gTypeStats.sendsIdentity;
	size_t *dynamicSlot = hot ? &gTypeStats.hotDynamic : &gTypeStats.sendsDynamic;

	// The two non-dynamic reasons are counted apart because they are not
	// interchangeable: a resolved receiver is a site optional types would make
	// MORE of, while an identity selector is a site they can never reach.
	// Order matches the classification: identity wins, exactly as in
	// SendClassify.h, so the buckets partition the sends with no overlap.
	if (identity) {
		(*identitySlot)++;
		return;
	}
	if (resolved) {
		(*staticSlot)++;
		return;
	}
	(*dynamicSlot)++;

	_Bool annotatable = 0;
	switch (receiver.type) {
	case OPERAND_ARG_VAR:
		// `self` is an ARG_VAR like any other in the bytecode (Scope.c defines
		// it at SELF_INDEX, ahead of the real parameters), and lumping it in
		// with the annotatable arguments is the single easiest way to overstate
		// this whole measurement: a self-send's receiver class is whatever
		// SUBCLASS is running the method, so no annotation on this method can
		// pin it. Split by index, not by guesswork.
		if (receiver.index == SELF_INDEX) {
			hot ? gTypeStats.hotSelf++ : gTypeStats.dynSelf++;
		} else {
			hot ? gTypeStats.hotArg++ : gTypeStats.dynArg++;
			annotatable = 1;
		}
		break;
	case OPERAND_TEMP_VAR:
		// Temps continue the same index space but carry their own operand
		// type, so no temp is ever self.
		hot ? gTypeStats.hotTemp++ : gTypeStats.dynTemp++;
		annotatable = 1;
		break;
	case OPERAND_INST_VAR:
		hot ? gTypeStats.hotInstVar++ : gTypeStats.dynInstVar++;
		break;
	case OPERAND_CONTEXT_VAR:
		hot ? gTypeStats.hotContextVar++ : gTypeStats.dynContextVar++;
		break;
	default:
		hot ? gTypeStats.hotOther++ : gTypeStats.dynOther++;
		break;
	}

	if (!hot || !annotatable || cell == NULL) {
		return;
	}
	// Same single ACQUIRE load the inliner uses (tryInlineSite): the state
	// object is immutable once published, and the STW sweep that frees
	// superseded states runs with every mutator parked, so one load then one
	// field read cannot tear.
	IcState *state = __atomic_load_n(&cell->state, __ATOMIC_ACQUIRE);
	switch (state->kind) {
	case IC_KIND_MONO: gTypeStats.annotMono++; break;
	case IC_KIND_PIC:  gTypeStats.annotPic++;  break;
	case IC_KIND_MEGA: gTypeStats.annotMega++; break;
	default:
		if (typeStatsHasExecuted(cell)) {
			gTypeStats.annotColdReset++;
		} else {
			gTypeStats.annotColdNever++;
		}
		break;
	}
}


static void typePrintShare(const char *label, size_t value, size_t total)
{
	if (total == 0) {
		printf("[TYPE] %-22s %8zu\n", label, value);
		return;
	}
	printf("[TYPE] %-22s %8zu  %5.1f%%\n", label, value, 100.0 * (double) value / (double) total);
}


void typePrintStats(void)
{
	size_t allSends = gTypeStats.sendsStatic + gTypeStats.sendsIdentity + gTypeStats.sendsDynamic;
	size_t hotSends = gTypeStats.hotStatic + gTypeStats.hotIdentity + gTypeStats.hotDynamic;
	size_t annot = gTypeStats.annotMono + gTypeStats.annotPic + gTypeStats.annotMega
		+ gTypeStats.annotColdNever + gTypeStats.annotColdReset;
	// A site only yields a dispatch if it EXECUTES: mega, plus the cold cells
	// whose witness proves they ran before the scavenge wiped them.
	size_t newInfo = gTypeStats.annotMega + gTypeStats.annotColdReset;
	size_t ran = annot - gTypeStats.annotColdNever;

	printf("[TYPE] --- ALL compiled methods (sites as written) ---\n");
	printf("[TYPE] %-22s %8zu\n", "methods", gTypeStats.methods);
	printf("[TYPE] %-22s %8zu\n", "sends", allSends);
	typePrintShare("  static", gTypeStats.sendsStatic, allSends);
	typePrintShare("  identity", gTypeStats.sendsIdentity, allSends);
	typePrintShare("  dynamic", gTypeStats.sendsDynamic, allSends);
	typePrintShare("    self", gTypeStats.dynSelf, gTypeStats.sendsDynamic);
	typePrintShare("    arg", gTypeStats.dynArg, gTypeStats.sendsDynamic);
	typePrintShare("    temp", gTypeStats.dynTemp, gTypeStats.sendsDynamic);
	typePrintShare("    instVar", gTypeStats.dynInstVar, gTypeStats.sendsDynamic);
	typePrintShare("    contextVar", gTypeStats.dynContextVar, gTypeStats.sendsDynamic);
	typePrintShare("    other", gTypeStats.dynOther, gTypeStats.sendsDynamic);

	printf("[TYPE] --- HOT methods (reached tier 1) ---\n");
	printf("[TYPE] %-22s %8zu\n", "hotMethods", gTypeStats.hotMethods);
	printf("[TYPE] %-22s %8zu\n", "sends", hotSends);
	typePrintShare("  static", gTypeStats.hotStatic, hotSends);
	typePrintShare("  identity", gTypeStats.hotIdentity, hotSends);
	typePrintShare("  dynamic", gTypeStats.hotDynamic, hotSends);
	typePrintShare("    self", gTypeStats.hotSelf, gTypeStats.hotDynamic);
	typePrintShare("    arg", gTypeStats.hotArg, gTypeStats.hotDynamic);
	typePrintShare("    temp", gTypeStats.hotTemp, gTypeStats.hotDynamic);
	typePrintShare("    instVar", gTypeStats.hotInstVar, gTypeStats.hotDynamic);
	typePrintShare("    contextVar", gTypeStats.hotContextVar, gTypeStats.hotDynamic);
	typePrintShare("    other", gTypeStats.hotOther, gTypeStats.hotDynamic);

	// The decision lines. `annotatable` is hot arg + temp receivers, the only
	// sites an M12-shape parameter annotation could reach at all.
	//
	// WARNING when reading this: "the compiler would learn something" and "the
	// program would get faster" are NOT the same bucket, and conflating them is
	// how a type system gets built for nothing. A site that never executed is a
	// site where a declared class is genuinely new information AND saves
	// exactly zero dispatches at runtime. Only two buckets carry speed:
	//   mega, and cold-that-actually-ran -> a real dispatch disappears
	//   mono                             -> the dispatch is ALREADY gone (the
	//                                       tier promoted or inlined it); what
	//                                       is left to win is the exact-class
	//                                       guard, 2 to 5 instructions
	// and one bucket is a hazard: pic sites see several classes, so an exact
	// declared type there is not an optimization, it is a program that raises.
	printf("[TYPE] --- what a declared type would buy (hot arg+temp) ---\n");
	printf("[TYPE] %-22s %8zu\n", "annotatable", annot);
	typePrintShare("  mono: guard only", gTypeStats.annotMono, annot);
	typePrintShare("  pic: type would LIE", gTypeStats.annotPic, annot);
	typePrintShare("  mega: real dispatch", gTypeStats.annotMega, annot);
	typePrintShare("  cold, ran: dispatch", gTypeStats.annotColdReset, annot);
	typePrintShare("  cold, never ran: none", gTypeStats.annotColdNever, annot);
	printf("[TYPE] ---\n");
	typePrintShare("removes a DISPATCH", newInfo, hotSends);
	typePrintShare("removes a GUARD only", gTypeStats.annotMono, hotSends);
	// Of the annotatable sites that ACTUALLY RAN, how many did the tier already
	// devirtualize? At 100% there is no dispatch left for a type system to
	// remove, whatever the raw site counts look like.
	typePrintShare("ran and already mono", gTypeStats.annotMono, ran);
	// Sanity line for the two cold buckets: if this is zero the witness never
	// fired and the split above is meaningless, not evidence of anything.
	printf("[TYPE] %-22s %8zu\n", "sites seen executing", gExecutedCount);
}


// Does this method contain at least one DYNAMIC send site? A method with none
// can never promote or inline anything at tier 1 (there is no IC feedback to
// act on), so it is pointless to give it an invocation counter and a prologue
// tier check. A dynamic send is exactly what generateSend turns into an IC
// cell: a non-identity selector whose receiver's class is not statically
// resolvable. Identity sends (==/~~/isNil/notNil) emit no dispatch, and
// static-receiver sends (nil/true/false/literal/super/block/thisContext) bake
// their target -- neither creates a cell. Pure bytecode analysis, sound and
// runtime-independent, mirroring the send classification the codegen and the
// inliner both use (jit/SendClassify.h).
_Bool tierMethodHasDynamicSend(CompiledCode *code)
{
	// ST_TYPE_STATS rides along here rather than adding a walk of its own: this
	// is already the one place that classifies EVERY send of EVERY framed
	// tier-0 method, exactly once, with no backend code involved. The census
	// needs the whole stream, so under the flag the early return below is
	// suppressed and the answer is accumulated instead.
	_Bool census = typeStatsEnabled();
	_Bool found = 0;
	if (census) {
		gTypeStats.methods++;
	}

	BytecodesIterator iterator;
	bytecodeInitIterator(&iterator, code->bytecodes, code->bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		Bytecode bytecode = bytecodeNext(&iterator);
		switch (bytecode) {
		case BYTECODE_COPY:
			bytecodeNextOperand(&iterator);
			bytecodeNextOperand(&iterator);
			break;
		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE: {
			uint16_t selectorIndex = bytecodeNextUint16(&iterator);
			uint8_t argsSize = bytecodeNextByte(&iterator);
			Operand receiver = bytecodeNextOperand(&iterator);
			for (uint8_t i = 0; i < argsSize; i++) {
				bytecodeNextOperand(&iterator);
			}
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				bytecodeNextOperand(&iterator);
			}
			RawObject *selector = compiledCodeLiteralAt(code, selectorIndex);
			_Bool identity = classifyIdentity(selector, argsSize) != IDENT_NONE;
			_Bool resolved = compiledCodeResolveOperandClass(code, receiver) != NULL;
			if (census) {
				typeStatsNoteSend(0, receiver, identity, resolved, NULL);
			}
			if (!identity && !resolved) {
				if (!census) {
					return 1;
				}
				found = 1;
			}
			break;
		}
		case BYTECODE_RETURN:
		case BYTECODE_OUTER_RETURN:
			bytecodeNextOperand(&iterator);
			break;
		case BYTECODE_JUMP:
			bytecodeNextInt32(&iterator);
			break;
		case BYTECODE_JUMP_NOT_MEMBER_OF:
			bytecodeNextUint16(&iterator);
			bytecodeNextOperand(&iterator);
			bytecodeNextInt32(&iterator);
			break;
		default:
			FAIL();
		}
	}
	return found;
}


// The invocation counter of this code hit zero: recompile its method at tier 1
// and republish. Reached only from the check that generateCode emits into a
// framed METHOD prologue, so compiledCode is always a CompiledMethod. The
// whole body runs under codegenLock: concurrent workers crossing the same
// threshold (the non-atomic decrement lets several observe zero) serialize
// here, and the TIER_TAG_FIRED latch makes the recompile once-only. Exec
// space never moves nor frees, so deriving the NativeCode from its entry is
// always valid; the METHOD object is movable, so it is re-read through a
// handle before the (allocating, GC-active) code generation.
void tierRecompile(uint8_t *insts)
{
	NativeCode *code = (NativeCode *) (insts - offsetof(NativeCode, insts));
	gTierStats.triggerCalls++;

	Heap *heap = CurrentThread.heap;
	heapCodegenLockEnter(heap);
	if ((code->tags & TIER_TAG_FIRED) == 0 && code->compiledCode != NULL) {
		code->tags |= TIER_TAG_FIRED; // latch FIRST: racing echoes skip above
		ASSERT(((RawObject *) code->compiledCode)->class == Handles.CompiledMethod->raw);

		HandleScope scope;
		openHandleScope(&scope);
		CompiledMethod *method = scopeHandle((RawCompiledMethod *) code->compiledCode);
		size_t promotedBefore = gTierStats.promotedSites;
		size_t inlinedBefore = gTierStats.inlinedSites;
		// Speculative inlining first (compiler/Optimizer.c): when any mono
		// site has an eligible leaf callee, the method's BYTECODES are
		// rewritten and compiled instead, with the site map standing in for
		// the positional cell pairing. Otherwise the original bytecodes
		// compile exactly as in tier M1.
		IcCell **siteMap = NULL;
		size_t siteMapSize = 0;
		CompiledMethod *optimized = optimizeMethod(method, code, &siteMap, &siteMapSize);
		NativeCode *fresh = generateMethodCodeTiered(
			optimized != NULL ? optimized : method, code, siteMap, siteMapSize);
		free(siteMap);
		if (gTierStats.promotedSites == promotedBefore
				&& gTierStats.inlinedSites == inlinedBefore) {
			// Nothing promoted (cells unlinked at recompile time, or every
			// site megamorphic): the fresh code would be tier-0 minus the
			// counter, and publishing it would only double this method's hot
			// code footprint (measured as pure icache/DSB pressure). Discard
			// it; the latch keeps the method from re-triggering. The block
			// codes regenerated as a side effect stay published: they are
			// reached through the baked immediates of whichever method code
			// created their Block objects, so the old method keeps pairing
			// with the old block code.
			gTierStats.discardedRecompiles++;
		} else {
			// RELEASE publish over the old pointer: new first-calls and IC
			// rebinds pick the tier-1 code; frames in flight finish in the
			// old code.
			compiledMethodSetNativeCode(method, fresh);
			gTierStats.recompiles++;
			// Adoption: without this, every caller bound to the old entry
			// keeps dispatching it (correct but tier-0) until the next
			// scavenge resets the cells. Flush THIS worker's lookup cache (a
			// stale entry would rebind retired cells straight back to the old
			// code) and retire every IC cell targeting the old entry; peer
			// workers with their own stale TLS entries converge at their next
			// epoch flush, documented staleness.
			flushLookupCache();
			icRetireCellsTargeting(code);
		}
		closeHandleScope(&scope, NULL);
	}
	heapCodegenLockLeave(heap);
}


void tierPrintStats(void)
{
	printf("[TIER] countedMethods      %zu\n", gTierStats.countedMethods);
	printf("[TIER] filteredMethods     %zu\n", gTierStats.filteredMethods);
	printf("[TIER] triggerCalls        %zu\n", gTierStats.triggerCalls);
	printf("[TIER] recompiles          %zu\n", gTierStats.recompiles);
	printf("[TIER] discardedRecompiles %zu\n", gTierStats.discardedRecompiles);
	printf("[TIER] promotedSites       %zu\n", gTierStats.promotedSites);
	printf("[TIER] unpromotedSites     %zu\n", gTierStats.unpromotedSites);
	printf("[TIER] inlinedSites        %zu\n", gTierStats.inlinedSites);
	printf("[TIER] directCalls         %zu\n", gTierStats.directCalls);
	printf("[TIER] guardFails          %zu\n", gTierStats.guardFails);
}
