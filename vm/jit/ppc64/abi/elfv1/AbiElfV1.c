// The ppc64 ELFv1 ABI instance. Exports ONLY elfv1-suffixed symbols, and ,
// unlike the x64 instance files, is deliberately HOST-INDEPENDENT: every
// emitter builds explicitly big-endian instruction words into an
// AssemblerBuffer and the fiber PRIME is plain C, so the x86 dev host links
// this TU into its own `st` and golden-tests the hooks natively
// (ST_PPC64_EMIT_TEST; bring-up rung 1 in PORTING.md). Nothing here executes
// POWER code; the one truly arch-only piece, the fiber switch asm, lives in
// FiberElfV1.c and is stubbed on foreign hosts.
//
// The generic bindings (gPpc64Abi, asmLoadTls, TargetFiber.h names) live in
// AbiElfV1Bind.c, which only a real ppc64 build links.
//
// This instance emits BIG-ENDIAN instruction words wherever it is compiled
// (the byte order belongs to the TARGET, not the build host).
#define ST_PPC64_EMIT_BE 1
#include "jit/ppc64/Abi.h"
#include "jit/ppc64/abi/elfv1/FiberElfV1.h"
#include "jit/ppc64/PrimFrame.h"
#include "core/Assert.h"
#include "core/Thread.h"
#include "core/StackFrame.h"
#include <string.h>

// C integer-argument registers, ELFv1 order.
static const uint8_t ElfV1ArgRegs[] = { R3, R4, R5, R6, R7, R8_PPC, R9_PPC, R10_PPC };

// ELFv1 callee-saved map, indexed by Register 0..31. r1 (SP), r2 (TOC) and
// r13 (thread pointer) are RESERVED, marked saved so the invariant check
// never lets them into a spill list; the JIT must not allocate them at all.
static const _Bool ElfV1CalleeSaved[32] = {
	/* r0  */ 0, /* r1  */ 1, /* r2  */ 1, /* r3  */ 0,
	/* r4  */ 0, /* r5  */ 0, /* r6  */ 0, /* r7  */ 0,
	/* r8  */ 0, /* r9  */ 0, /* r10 */ 0, /* r11 */ 0,
	/* r12 */ 0, /* r13 */ 1, /* r14 */ 1, /* r15 */ 1,
	/* r16 */ 1, /* r17 */ 1, /* r18 */ 1, /* r19 */ 1,
	/* r20 */ 1, /* r21 */ 1, /* r22 */ 1, /* r23 */ 1,
	/* r24 */ 1, /* r25 */ 1, /* r26 */ 1, /* r27 */ 1,
	/* r28 */ 1, /* r29 */ 1, /* r30 */ 1, /* r31 */ 1,
};

// Registers the JIT may hold live values in that ELFv1 clobbers across a C
// call. ORDER IS CONTRACT (golden-pinned): the volatile subset of
// Ppc64AvailableRegs (r9) plus the dispatch-scratch extras r3-r8 (result,
// class/selector/size, extra scratch, mirroring x64's
// RAX/RCX/RDX/RSI/RDI/R8/R9 spill set). TMP/TMP2/TGT are per-instruction or
// per-send transients, never live across a slow-path C call, like x64's
// TMP(R10). The golden invariant check keeps this list honest against the
// pool.
static const uint8_t ElfV1CallerSavedSpill[] = {
	R3, R4, R5, R6, R7, R8_PPC, R9_PPC,
};

// High/low 16-bit halves of a 32-bit offset for addis+addi: the low half is
// SIGN-extended by addi, so the high half rounds up when bit 15 is set.
static ptrdiff_t elfv1Ha16(ptrdiff_t v)
{
	return (v + 0x8000) >> 16;
}

static ptrdiff_t elfv1Lo16(ptrdiff_t v)
{
	return (int16_t) (v & 0xFFFF);
}

// Load the address of the running worker's initial-exec TLS variable into
// `dst`: dst = r13 + tpoff. tpoff is computed at runtime relative to r13
// (Thread.c targetThreadPointer), so the TLS ABI's 0x7000 bias cancels out
// of the subtraction, no bias handling here. Fixed 2-instruction shape for
// any 32-bit tpoff (positive or negative).
static void elfv1EmitLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	ASSERT(-0x7FFF8000 <= tpoff && tpoff <= 0x7FFF7FFF);
	asmAddis(buffer, dst, R13_PPC, elfv1Ha16(tpoff));
	asmAddi(buffer, dst, dst, elfv1Lo16(tpoff));
}

// Call an absolute C function through its ELFv1 .opd function DESCRIPTOR
// {entry, TOC, environ}: save the caller's r2 in its ABI slot, load the real
// entry and the callee TOC (and the environment, as compilers do for
// indirect calls) from the descriptor, dispatch via CTR, restore r2.
// r11 is the ABI-conventional descriptor/environment register, used BY ROLE
// here, independent of the VM's TMP placeholder choice.
static void elfv1EmitCallCFunction(AssemblerBuffer *buffer, intptr_t cFunction)
{
	asmLi64(buffer, R11_PPC, (uint64_t) cFunction);
	asmStd(buffer, R2, ELFV1_NV_FRAME_TOC, R1);
	asmLd(buffer, R0, 0, R11_PPC);
	asmLd(buffer, R2, 8, R11_PPC);
	asmMtctr(buffer, R0);
	asmLd(buffer, R11_PPC, 16, R11_PPC);
	asmBctrl(buffer);
	asmLd(buffer, R2, ELFV1_NV_FRAME_TOC, R1);
}

// The CCALL-primitive trampoline body, FUSED for ELFv1: builds the temp
// frame + ABI frame itself instead of reusing generateCCall, because ELFv1
// returns the 16-byte PrimitiveResult through a HIDDEN sret pointer in r3
// (every argument shifts right by one, the Win64-class silent break,
// PORTING.md) and the buffer at 96(r1) must be read back BEFORE the frame
// teardown: below r1 is signal-clobberable memory. Calls this instance's own
// static emitters (not the gPpc64Abi generics), so the TU stays linkable on
// a foreign golden host.
static void elfv1EmitCCallPrimitive(AssemblerBuffer *buffer,
	struct CodeGenerator *generator, intptr_t cFunction, size_t argsSize,
	AssemblerLabel *failLabel)
{
	(void) generator;   // the fused body never re-enters the code generator

	primFramedPrologue(buffer);
	asmPush(buffer, CTX);
	asmMr(buffer, R15_PPC, TGT);       // the x64 R11->R13 native-code dance
	asmRldicr(buffer, R1, R1, 0, 59);  // 16-byte alignment
	asmStdu(buffer, R1, -112, R1);     // ELFv1 header + param save area

	// exit frame for the C call (RUNNING worker via TLS)
	elfv1EmitLoadTls(buffer, TMP, gCurrentThreadTpoff);
	asmLd(buffer, TMP, offsetof(Thread, stackFramesTail), TMP);
	asmStd(buffer, FP, offsetof(EntryStackFrame, exit), TMP);

	// marshal: r3 = hidden sret pointer, Smalltalk arg i (at (i+2)*8(FP)
	// after the two pushes) -> C arg i+1 (r4..)
	asmAddi(buffer, R3, R1, 96);
	for (size_t i = 0; i < argsSize; i++) {
		asmLd(buffer, (Register) (R4 + i), (ptrdiff_t) (i + 2) * sizeof(intptr_t), FP);
	}

	elfv1EmitCallCFunction(buffer, cFunction);

	// decode the PrimitiveResult from the sret buffer BEFORE teardown
	asmLd(buffer, R3, 96, R1);                       // value
	asmLd(buffer, R4, 96 + sizeof(Value), R1);       // failed

	asmLd(buffer, CTX, -(ptrdiff_t) sizeof(intptr_t), FP);
	primFramedEpilogue(buffer);

	asmCmpdi(buffer, 0, R4, 0);
	asmBne(buffer, failLabel);
}

// Entry-stub register save/restore: the full ELFv1 nonvolatile set in the
// shared ELFV1_NV frame shape (see FiberElfV1.h). Must move r1 by exactly
// AbiPpc64ElfV1.entrySavedRegsSize.
static void elfv1EmitEntrySaveRegs(AssemblerBuffer *buffer)
{
	asmMflr(buffer, R0);
	asmStd(buffer, R0, ELFV1_HEADER_LR_SAVE, R1);
	asmMfcr(buffer, R0);
	asmStw(buffer, R0, ELFV1_HEADER_CR_SAVE, R1);
	asmStdu(buffer, R1, -ELFV1_NV_FRAME_SIZE, R1);
	asmStd(buffer, R2, ELFV1_NV_FRAME_TOC, R1);
	for (int i = 0; i < 18; i++) {
		asmStd(buffer, (Register) (R14_PPC + i), ELFV1_NV_FRAME_GPRS + i * 8, R1);
	}
	for (int i = 0; i < 18; i++) {
		asmStfd(buffer, 14 + i, ELFV1_NV_FRAME_FPRS + i * 8, R1);
	}
}

static void elfv1EmitEntryRestoreRegs(AssemblerBuffer *buffer)
{
	for (int i = 0; i < 18; i++) {
		asmLd(buffer, (Register) (R14_PPC + i), ELFV1_NV_FRAME_GPRS + i * 8, R1);
	}
	for (int i = 0; i < 18; i++) {
		asmLfd(buffer, 14 + i, ELFV1_NV_FRAME_FPRS + i * 8, R1);
	}
	asmLd(buffer, R2, ELFV1_NV_FRAME_TOC, R1);
	asmAddi(buffer, R1, R1, ELFV1_NV_FRAME_SIZE);
	asmLd(buffer, R0, ELFV1_HEADER_LR_SAVE, R1);
	asmMtlr(buffer, R0);
	asmLwz(buffer, R0, ELFV1_HEADER_CR_SAVE, R1);
	asmMtcrf(buffer, 0xFF, R0);
}

// Prime a fresh fiber stack: build the exact ELFV1_NV frame the restore path
// of fiberSwitchElfV1 expects, so the first switch into it "returns" into
// `entry` with r14-r31/f14-f31 zeroed, CR zeroed, r2 = the entry's TOC and a
// null-terminated back chain. Under ELFv1 `entry` (a C function pointer) IS
// a descriptor pointer, the entry address and TOC come from dereferencing
// it at prime time (host C, no emitted code). Touches < one page below
// `top` (the caller commits at least one page).
void *fiberPrimeStackElfV1(void *top, void (*entry)(void))
{
	const uintptr_t *descriptor = (const uintptr_t *) (uintptr_t) entry;
	uintptr_t base = ((uintptr_t) top - 64) & ~(uintptr_t) 15;
	uintptr_t sp = base - ELFV1_NV_FRAME_SIZE;

	memset((void *) sp, 0, (uintptr_t) top - sp);
	*(uintptr_t *) sp = base;                                    // back chain
	*(uintptr_t *) (sp + ELFV1_NV_FRAME_TOC) = descriptor[1];    // r2 = callee TOC
	*(uintptr_t *) (base + ELFV1_HEADER_LR_SAVE) = descriptor[0]; // LR = real entry
	return (void *) sp;
}

const Ppc64Abi AbiPpc64ElfV1 = {
	.name = "elfv1",
	.argRegs = ElfV1ArgRegs,
	.argRegsCount = sizeof(ElfV1ArgRegs),
	.calleeSaved = ElfV1CalleeSaved,
	.callerSavedSpill = ElfV1CallerSavedSpill,
	.callerSavedSpillCount = sizeof(ElfV1CallerSavedSpill),
	.paramSaveArea = 64,
	.cCallFrameSize = 112,
	.entrySavedRegsSize = ELFV1_NV_FRAME_SIZE,
	.emitEntrySaveRegs = elfv1EmitEntrySaveRegs,
	.emitEntryRestoreRegs = elfv1EmitEntryRestoreRegs,
	.emitLoadTls = elfv1EmitLoadTls,
	.emitCallCFunction = elfv1EmitCallCFunction,
	.emitCCallPrimitive = elfv1EmitCCallPrimitive,
	.fiberSwitch = fiberSwitchElfV1,
	.fiberPrimeStack = fiberPrimeStackElfV1,
};
