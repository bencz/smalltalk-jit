#include "jit/Tier.h"
#include "jit/CodeGenerator.h"
#include "jit/SendClassify.h"
#include "compiler/Optimizer.h"
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


size_t *tierAllocCounter(void)
{
	size_t *cell = malloc(sizeof(size_t));
	*cell = tierThreshold();
	return cell;
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
			uint8_t selectorIndex = bytecodeNextByte(&iterator);
			uint8_t argsSize = bytecodeNextByte(&iterator);
			Operand receiver = bytecodeNextOperand(&iterator);
			for (uint8_t i = 0; i < argsSize; i++) {
				bytecodeNextOperand(&iterator);
			}
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				bytecodeNextOperand(&iterator);
			}
			RawObject *selector = compiledCodeLiteralAt(code, selectorIndex);
			if (classifyIdentity(selector, argsSize) == IDENT_NONE
					&& compiledCodeResolveOperandClass(code, receiver) == NULL) {
				return 1;
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
			bytecodeNextByte(&iterator);
			bytecodeNextOperand(&iterator);
			bytecodeNextInt32(&iterator);
			break;
		default:
			FAIL();
		}
	}
	return 0;
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
