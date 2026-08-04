// The tier-2 pipeline, end to end: a CodeUnit becomes executable machine code.
//
//   bytecode -> SSA -> optimize -> LIR -> allocate -> verify -> emit -> publish
//
// A SEPARATE TRANSLATION UNIT from jit/Jit.c, and that is structural rather
// than tidy: gate levels 3, 8 and 9 hand-link Jit.c with half a dozen files and
// no tier 2 at all, so every reference from Jit.c to this half has to be a weak
// symbol. The same seam jit/Tier2DryRun.c uses.
//
// IT REFUSES RATHER THAN GUESSES, at four different points, and each answers a
// different question:
//
//   ssaBuild        an opcode the IR does not model
//   lirLower        an IR operation this backend cannot select
//   the allocator   a register pool that cannot serve the method
//   the verifier    an allocation that does not hold together
//
// Any of them means tier 1's output stands, which is always available and
// always correct. Nothing here is a fallback in the sense of being worse; tier 1
// is the baseline and tier 2 is an attempt to beat it.

#include "jit/SsaBackend.h"
#include "jit/SsaBuild.h"
#include "jit/Passes.h"
#include "jit/Lower.h"
#include "jit/RegAlloc.h"
#include "jit/SsaEmitter.h"
#include "jit/InlineCache.h"
#include "jit/Specialize.h"
#include "core/Class.h"
#include "jit/Deopt.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


NativeCode *ssaCompile(const SsaEmitterOps *ops, CodeUnit *unit,
	NativeCode *tier1, const char **refused, PassStats *stats)
{
	const char *reason = NULL;
	Opcode unsupported = OP_COUNT;
	IrFunction *ir = ssaBuild(unit, &unsupported);
	if (ir == NULL) {
		reason = opcodeName(unsupported);
		goto refuse;
	}
	// THE PROFILE, read once and handed to the optimizer as data. It comes from
	// TIER 1's cells, which is the only place it exists: this compilation is
	// happening because those sites ran. A method compiled with no tier 1 -- the
	// dry run does exactly that -- gets no table and every send stays a send,
	// which is correct and is what "the profile has not happened yet" means.
	SiteSpecialization *sites = specializeFor(tier1, NULL);
	IrProfile profile;
	memset(&profile, 0, sizeof(profile));
	profile.sites = sites;
	// The LENGTH comes from the same unit that sized the array, which is tier
	// 1's and not this compilation's: they are the same bytecode, and taking the
	// count from the other one would be an array length derived from a second
	// source that merely happens to agree.
	profile.siteCount = sites != NULL ? tier1->unit->instructionCount : 0;
	profile.smallIntegerClass = gClassIndexByTag[VALUE_INT];
	// --deopt-stress, the internal oracle of ADR 0002: every speculation in this
	// method is made unsatisfiable, so every execution of it leaves optimized
	// code and has to arrive at tier 1's answer.
	profile.stressGuards = deoptStressEnabled();
	profile.stressSkip = deoptStressSkip();
	PassStats passes = irOptimize(ir, &profile);
	if (stats != NULL) {
		*stats = passes;
	}
	free(sites);

	LirFunction *lir = lirLower(ir, ops->abi, tier1, &reason);
	if (lir == NULL) {
		irDestroy(ir);
		goto refuse;
	}

	RegAllocStats allocation = lirAllocateRegisters(lir);
	if (allocation.failed || !lirVerifyAllocation(lir)) {
		reason = allocation.failed ? "register allocation" : "allocation verifier";
		lirDestroy(lir);
		irDestroy(ir);
		goto refuse;
	}

	SsaEmitter *emitter = ssaEmit(ops, lir, tagPtr(Handles.nil.raw));

	NativeCode *code = calloc(1, sizeof(NativeCode));
	ASSERT(code != NULL);
	code->unit = unit;
	code->frameSlots = lir->frameSlots;
	// RECORDED, not re-derived, for the reason tier 1 gives at the same line:
	// two derivations that could disagree are a callee reading its arguments out
	// of registers nobody wrote.
	code->wide = maWideForArity(ops->abi, unit->argumentCount);
	// THE CELLS ARE TIER 1'S. Sharing keeps a site's profile cumulative across
	// the two tiers; a second set would split every site's history in half and
	// leave a deoptimization landing on caches that had gone cold.
	code->cells = tier1 != NULL ? tier1->cells : NULL;

	// The bci map. Tier 2 reorders and merges blocks, so not every bytecode
	// index has machine code of its own any more; what survives is the offset
	// of each BLOCK, which is what OSR and a backtrace actually ask for. An
	// index with no block of its own answers the block that contains it.
	code->machineOffsetAt = calloc(unit->instructionCount == 0 ? 1
		: unit->instructionCount, sizeof(uint32_t));
	ASSERT(code->machineOffsetAt != NULL);
	for (LirBlock *block = lir->blocks; block != NULL; block = block->next) {
		if (block->label < unit->instructionCount && block->first != NULL) {
			code->machineOffsetAt[block->label] = block->first->codeOffset;
		}
	}

	// THE FRAME DESCRIPTION IS NOT ONE MAP HERE, which is the difference tier 1
	// anticipated: it keeps raw values in slots, so what a slot holds changes
	// from safepoint to safepoint. The maps the allocator built are anchored to
	// machine offsets now that emission has assigned them.
	//
	// The one on the NativeCode is the map at the METHOD'S ENTRY, which is what
	// a walk finds when it lands anywhere no call covers.
	size_t mapBytes = sizeof(FrameMap) + frameMapByteCount(code->frameSlots);
	code->frameMap = calloc(1, mapBytes);
	ASSERT(code->frameMap != NULL);
	code->frameMap->codeOffset = 0;
	code->frameMap->slotCount = code->frameSlots;
	code->frameMap->byteCount = (uint16_t) frameMapByteCount(code->frameSlots);
	for (uint16_t slot = 0; slot < lir->parameterSlots; slot++) {
		frameMapSetKind(code->frameMap, slot, SLOT_POINTER);
	}

	if (ops == ssaHostBackend()) {
		size_t size;
		const uint8_t *bytes = ssaEmitterBytes(emitter, &size);
		code->entry = codeSpaceAllocate(size);
		codeSpacePublish(code->entry, bytes, size);
		code->size = size;
	} else {
		size_t size;
		const uint8_t *bytes = ssaEmitterBytes(emitter, &size);
		code->size = size;
		code->entry = malloc(size);
		ASSERT(code->entry != NULL);
		memcpy(code->entry, bytes, size);
	}

	// ANCHOR THE SITES, now that emission has assigned every instruction an
	// offset, and take ownership of them: they are malloc'd rather than
	// arena-allocated precisely so they can outlive the LIR, because the guard
	// sequence baked their addresses into executable memory.
	code->deoptSiteCount = lir->deoptSiteCount;
	code->deoptSites = lir->deoptSites;
	for (LirBlock *block = lir->blocks; block != NULL; block = block->next) {
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			if (it->deoptSite != NULL) {
				it->deoptSite->codeOffset = it->codeOffset;
			}
		}
	}
	lir->deoptSitesTransferred = 1;

	ssaEmitterDestroy(emitter);
	lirDestroy(lir);
	irDestroy(ir);

	compiledCodeRegister(code);
	return code;

refuse:
	if (refused != NULL) {
		*refused = reason != NULL ? reason : "?";
	}
	return NULL;
}
