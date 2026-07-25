// Shared delegates from generic x64 emitter names into the selected ABI
// instance (link-time bound via gX64Abi — see Abi.h). Kept as real functions
// (not header statics) so there is exactly ONE binding per process and the
// 12 asmLoadTls emit sites across the backend stay untouched by ABI swaps.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/Abi.h"
#include "jit/TargetCodePatch.h"
#include "jit/TargetEntry.h"
#include "core/CompiledCode.h"
#include "core/Endian.h"


void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	gX64Abi->emitLoadTls(buffer, dst, tpoff);
}


// Baked pointers live as the contiguous imm64 of a movabs on x64 — plain
// (alignment-safe) 8-byte accesses. See jit/TargetCodePatch.h.
uint64_t targetReadCodePointer(const uint8_t *site)
{
	return loadU64(site);
}


void targetWriteCodePointer(uint8_t *site, uint64_t value)
{
	storeU64(site, value);
}


// Speculation-guard poison (jit/SpecSite.h). asmJ always emits the FIXED
// 6-byte near form `0F 8x rel32` (never the 2-byte short form), so the site is
// known to be exactly:
//     site+0: 0F  site+1: 80+cc  site+2..5: rel32   ; target = site+6+rel32
// The unconditional near jump `E9 rel32'` is 5 bytes and measures its
// displacement from site+5, so the same target needs rel32' = rel32 + 1, and
// the freed trailing byte becomes a NOP. Rewriting the branch (rather than
// spoiling a baked class immediate) is what makes one poison work for every
// receiver shape: an immediate-class guard is a bare tag test that bakes no
// class at all.
void targetPoisonGuardBranch(uint8_t *site)
{
	// IDEMPOTENT: invalidation is per redefinition, not per site, so a site
	// already poisoned by an earlier redefinition is simply left alone. Its
	// guard can never come back: the fallback is the full send and only a
	// fresh compilation restores speculation.
	if (site[0] == 0xE9) {
		return;
	}
	ASSERT(site[0] == 0x0F && (site[1] & 0xF0) == 0x80);
	int32_t rel = (int32_t) loadU32(site + 2);
	site[0] = 0xE9;
	storeU32(site + 1, (uint32_t) (rel + 1));
	site[5] = 0x90;
}


// SPEC_STATIC poison: the site is the 5-byte `E9 rel32` that skips the send's
// inline re-resolve thunk. Zeroing the displacement makes it jump to the very
// next instruction, which is the thunk. Idempotent by construction: writing
// zero twice is writing zero.
void targetPoisonStaticSkip(uint8_t *site)
{
	ASSERT(site[0] == 0xE9);
	storeU32(site + 1, 0);
}


// C -> JIT entry: on x64 the stub's code address IS a callable C function
// pointer. See jit/TargetEntry.h (ppc64 ELFv1 needs a descriptor instead).
Value targetCallSmalltalkEntry(void *entryStubInsts, void *arg0, void *arg1,
	Value *args, struct Thread *thread)
{
	union PointerConverter converter;
	converter.object_pointer = entryStubInsts;
	return converter.function_pointer((Value) arg0, (Value) arg1, args, thread);
}
