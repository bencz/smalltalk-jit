// The ppc64le ELFv2 ABI instance. Exports ONLY elfv2-suffixed symbols, and,
// like its ELFv1 sibling, is deliberately HOST-INDEPENDENT: every emitter
// builds explicitly little-endian instruction words into an AssemblerBuffer
// and the fiber PRIME is plain C, so the x86 dev host links this TU into its
// own `st` and golden-tests the hooks natively (ST_PPC64LE_EMIT_TEST; bring-up
// rung 1 in PORTING.md). Nothing here executes POWER code; the one truly
// arch-only piece, the fiber switch asm, lives in FiberElfV2.c and is stubbed
// on foreign hosts. Keep it that way: no `register asm("r2")`, no target
// intrinsics.
//
// The generic bindings (gPpc64Abi, asmLoadTls, TargetFiber.h names) live in
// AbiElfV2Bind.c, which only a real ppc64le build links.
//
// Every ELFv2 fact encoded here was derived from gcc's own output rather than
// from the spec, with the BE cross as a control: vm/jit/ppc64/DESIGN-elfv2.md.
// This instance emits LITTLE-ENDIAN instruction words wherever it is
// compiled (the byte order belongs to the TARGET, not the build host).
#define ST_PPC64_EMIT_LE 1
#include "jit/ppc64/Abi.h"
#include "jit/ppc64/abi/elfv2/FiberElfV2.h"

// Target-only service this hook drives through the OPAQUE generator (see the
// emitCCallPrimitive contract in Abi.h): declared locally because instance
// TUs must not include jit/CodeGenerator.h. On a foreign golden host the
// symbol resolves to the host backend and is never called.
void generateCCall(struct CodeGenerator *generator, intptr_t cFunction,
	size_t argsSize, _Bool storeIp);
#include "core/Assert.h"
#include <string.h>

// C integer-argument registers, ELFv2 order (identical to ELFv1: a
// generic-POWER fact, not an ABI-version one).
static const uint8_t ElfV2ArgRegs[] = { R3, R4, R5, R6, R7, R8_PPC, R9_PPC, R10_PPC };

// ELFv2 callee-saved map, indexed by Register 0..31. r1 (SP), r2 (TOC) and
// r13 (thread pointer) are RESERVED, marked saved so the invariant check
// never lets them into a spill list; the JIT must not allocate them at all.
// ELFv2 only requires r2 across GLOBAL calls, but reserving it outright stays
// correct and matches the BE backend.
static const _Bool ElfV2CalleeSaved[32] = {
	/* r0  */ 0, /* r1  */ 1, /* r2  */ 1, /* r3  */ 0,
	/* r4  */ 0, /* r5  */ 0, /* r6  */ 0, /* r7  */ 0,
	/* r8  */ 0, /* r9  */ 0, /* r10 */ 0, /* r11 */ 0,
	/* r12 */ 0, /* r13 */ 1, /* r14 */ 1, /* r15 */ 1,
	/* r16 */ 1, /* r17 */ 1, /* r18 */ 1, /* r19 */ 1,
	/* r20 */ 1, /* r21 */ 1, /* r22 */ 1, /* r23 */ 1,
	/* r24 */ 1, /* r25 */ 1, /* r26 */ 1, /* r27 */ 1,
	/* r28 */ 1, /* r29 */ 1, /* r30 */ 1, /* r31 */ 1,
};

// Registers the JIT may hold live values in that ELFv2 clobbers across a C
// call. ORDER IS CONTRACT (golden-pinned): the volatile subset of
// Ppc64AvailableRegs (r9) plus the dispatch-scratch extras r3-r8 (result,
// class/selector/size, extra scratch, mirroring x64's
// RAX/RCX/RDX/RSI/RDI/R8/R9 spill set). TMP/TMP2/TGT are per-instruction or
// per-send transients, never live across a slow-path C call, like x64's
// TMP(R10). The golden invariant check keeps this list honest against the
// pool.
static const uint8_t ElfV2CallerSavedSpill[] = {
	R3, R4, R5, R6, R7, R8_PPC, R9_PPC,
};

// High/low 16-bit halves of a 32-bit offset for addis+addi: the low half is
// SIGN-extended by addi, so the high half rounds up when bit 15 is set.
static ptrdiff_t elfv2Ha16(ptrdiff_t v)
{
	return (v + 0x8000) >> 16;
}

static ptrdiff_t elfv2Lo16(ptrdiff_t v)
{
	return (int16_t) (v & 0xFFFF);
}

// Load the address of the running worker's initial-exec TLS variable into
// `dst`: dst = r13 + tpoff. tpoff is computed at runtime relative to r13
// (Thread.c targetThreadPointer), so the TLS ABI's 0x7000 bias cancels out
// of the subtraction: no bias handling here. Fixed 2-instruction shape for
// any 32-bit tpoff (positive or negative). Byte-for-byte the ELFv1 hook: TLS
// on ppc64 is r13-based under both ABIs.
static void elfv2EmitLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	ASSERT(-0x7FFF8000 <= tpoff && tpoff <= 0x7FFF7FFF);
	asmAddis(buffer, dst, R13_PPC, elfv2Ha16(tpoff));
	asmAddi(buffer, dst, dst, elfv2Lo16(tpoff));
}

// Call an absolute C function. ELFv2 has NO function descriptors: the target
// address itself goes in r12, which the ABI mandates so the callee's
// global-entry prologue (`addis 2,12,.TOC.-.LCF@ha; addi 2,2,...@l`) can derive
// its own TOC; then dispatch via CTR with the caller's r2 saved around the
// call, exactly as gcc frames an indirect call (DESIGN.md item 2).
//
// WARNING: r12 is the VM's TGT. That is a harmony (a send target and a C-call
// target want the same register), but it means TGT must be DEAD at every call
// site, where the ELFv1 hook only required TMP dead.
static void elfv2EmitCallCFunction(AssemblerBuffer *buffer, intptr_t cFunction)
{
	asmLi64(buffer, R12_PPC, (uint64_t) cFunction);
	asmStd(buffer, R2, ELFV2_NV_FRAME_TOC, R1);
	asmMtctr(buffer, R12_PPC);
	asmBctrl(buffer);
	asmLd(buffer, R2, ELFV2_NV_FRAME_TOC, R1);
}

// The CCALL-primitive trampoline body, the SysV-like shape: ELFv2 returns
// the 16-byte PrimitiveResult in r3:r4 = value:failed with no hidden sret
// pointer and no argument shift, so generateCCall can own the frame as
// usual and nothing must be read back before teardown (the reason ELFv1's
// body is fused instead).
//
// The marshal runs BEFORE generateCCall builds its frame, so r1 is still the
// primitive's frameless entry SP. The x64 hook reads slot i at
// [RSP + (i+1)*8] because `call` pushed a return address; POWER keeps the
// return address in LR, so arg i really sits at i*8(r1). This is the same +1
// bias that fillVar has to undo (BE bring-up bug #2), showing up in a second
// place. After the call r3 = value is already where the VM wants it (our
// result register), so just test the flag and branch.
static void elfv2EmitCCallPrimitive(AssemblerBuffer *buffer,
	struct CodeGenerator *generator, intptr_t cFunction, size_t argsSize,
	AssemblerLabel *failLabel)
{
	ASSERT(argsSize <= sizeof(ElfV2ArgRegs));
	for (size_t i = 0; i < argsSize; i++) {
		asmLd(buffer, (Register) ElfV2ArgRegs[i], (ptrdiff_t) (i * sizeof(intptr_t)), R1);
	}
	asmMr(buffer, R15_PPC, TGT);       // the x64 R11->R13 native-code dance
	generateCCall(generator, cFunction, argsSize, 0);
	asmCmpdi(buffer, 0, R4, 0);
	asmBne(buffer, failLabel);
}

// Entry-stub register save/restore: the full ELFv2 nonvolatile set in the
// shared ELFV2_NV frame shape (see FiberElfV2.h). Must move r1 by exactly
// AbiPpc64ElfV2.entrySavedRegsSize.
static void elfv2EmitEntrySaveRegs(AssemblerBuffer *buffer)
{
	asmMflr(buffer, R0);
	asmStd(buffer, R0, ELFV2_HEADER_LR_SAVE, R1);
	asmMfcr(buffer, R0);
	asmStw(buffer, R0, ELFV2_HEADER_CR_SAVE, R1);
	asmStdu(buffer, R1, -ELFV2_NV_FRAME_SIZE, R1);
	asmStd(buffer, R2, ELFV2_NV_FRAME_TOC, R1);
	for (int i = 0; i < 18; i++) {
		asmStd(buffer, (Register) (R14_PPC + i), ELFV2_NV_FRAME_GPRS + i * 8, R1);
	}
	for (int i = 0; i < 18; i++) {
		asmStfd(buffer, 14 + i, ELFV2_NV_FRAME_FPRS + i * 8, R1);
	}
}

static void elfv2EmitEntryRestoreRegs(AssemblerBuffer *buffer)
{
	for (int i = 0; i < 18; i++) {
		asmLd(buffer, (Register) (R14_PPC + i), ELFV2_NV_FRAME_GPRS + i * 8, R1);
	}
	for (int i = 0; i < 18; i++) {
		asmLfd(buffer, 14 + i, ELFV2_NV_FRAME_FPRS + i * 8, R1);
	}
	asmLd(buffer, R2, ELFV2_NV_FRAME_TOC, R1);
	asmAddi(buffer, R1, R1, ELFV2_NV_FRAME_SIZE);
	asmLd(buffer, R0, ELFV2_HEADER_LR_SAVE, R1);
	asmMtlr(buffer, R0);
	asmLwz(buffer, R0, ELFV2_HEADER_CR_SAVE, R1);
	asmMtcrf(buffer, 0xFF, R0);
}

// Prime a fresh fiber stack: build the exact ELFV2_NV frame the restore path
// of fiberSwitchElfV2 expects, so the first switch into it "returns" into
// `entry` with r14-r31/f14-f31 zeroed, CR zeroed and a null-terminated back
// chain. Touches less than one page below `top` (the caller commits at least
// one page).
//
// Two deltas from the ELFv1 prime, both from ELFv2 having no descriptors:
//   1. `entry` IS the code address, so there is nothing to dereference.
//   2. The r2 slot is NOT seeded. Under ELFv1 the prime had to plant the
//      callee TOC because the switch restores r2 before returning; under ELFv2
//      the entry's global-entry prologue recomputes r2 from r12, and the
//      switch delivers r12 by copying the LR value it is about to branch to
//      (see FiberElfV2.c). Seeding r2 from C would need `register asm("r2")`,
//      which would break this TU's host-independence for zero benefit.
void *fiberPrimeStackElfV2(void *top, void (*entry)(void))
{
	uintptr_t base = ((uintptr_t) top - 64) & ~(uintptr_t) 15;
	uintptr_t sp = base - ELFV2_NV_FRAME_SIZE;

	memset((void *) sp, 0, (uintptr_t) top - sp);
	*(uintptr_t *) sp = base;                                        // back chain
	*(uintptr_t *) (base + ELFV2_HEADER_LR_SAVE) = (uintptr_t) entry; // LR = entry
	return (void *) sp;
}

const Ppc64Abi AbiPpc64ElfV2 = {
	.name = "elfv2",
	.argRegs = ElfV2ArgRegs,
	.argRegsCount = sizeof(ElfV2ArgRegs),
	.calleeSaved = ElfV2CalleeSaved,
	.callerSavedSpill = ElfV2CallerSavedSpill,
	.callerSavedSpillCount = sizeof(ElfV2CallerSavedSpill),
	.paramSaveArea = 0,
	.cCallFrameSize = 32,
	.entrySavedRegsSize = ELFV2_NV_FRAME_SIZE,
	.emitEntrySaveRegs = elfv2EmitEntrySaveRegs,
	.emitEntryRestoreRegs = elfv2EmitEntryRestoreRegs,
	.emitLoadTls = elfv2EmitLoadTls,
	.emitCallCFunction = elfv2EmitCallCFunction,
	.emitCCallPrimitive = elfv2EmitCCallPrimitive,
	.fiberSwitch = fiberSwitchElfV2,
	.fiberPrimeStack = fiberPrimeStackElfV2,
};
