#ifndef PPC64_ABI_H
#define PPC64_ABI_H

// The ppc64 platform-ABI binding, mirroring vm/jit/x64/Abi.h: everything the
// JIT emits or executes that depends on the C calling convention lives behind
// one ops-struct. The backend serves BOTH POWER targets; which instance binds
// is the ST_ABI axis: ELFv1 on big-endian (AIX heritage: function
// descriptors, TOC save at 40(r1), 48-byte frame header + caller-allocated
// parameter save area) and ELFv2 on little-endian (local entry points, TOC
// save at 24(r1), 32-byte header, optional parameter save area).
//
// NOT parameterized here, by design (same rule as x64): the VM-INTERNAL
// calling convention (Smalltalk stack, TMP/CTX roles, allocation pool),
// those are backend facts, not ABI facts.
//
// Instances export ONLY abi-suffixed symbols (AbiPpc64ElfV1, AbiPpc64ElfV2,
// fiberSwitchElfV1, ...) and the instance TUs are HOST-INDEPENDENT (pure byte
// emitters + the C stack-priming), so a foreign build host can link BOTH and
// golden-test every hook natively (vm/tests/EmitGoldenPpc64.c and
// EmitGoldenPpc64le.c). The generic names (gPpc64Abi, asmLoadTls,
// fiberSwitchAsm, fiberTargetPrimeStack) live exclusively in the selected
// ABI's Abi<Abi>Bind.c, which only a real ppc64 build links.
#include "jit/ppc64/AssemblerPpc64.h"

struct CodeGenerator;

typedef struct Ppc64Abi {
	const char *name;                    // "elfv1" / "elfv2"

	// C integer-argument registers, true C-ABI order: r3..r10.
	const uint8_t *argRegs;
	uint8_t argRegsCount;

	// Callee-saved map indexed by Register 0..31 (r1 SP, r2 TOC and r13 TP
	// are RESERVED and marked saved; the JIT must never allocate them).
	const _Bool *calleeSaved;

	// Registers the JIT may hold live values in that this C ABI clobbers,
	// spilled around slow-path C calls. ORDER IS CONTRACT (golden-pinned).
	const uint8_t *callerSavedSpill;
	uint8_t callerSavedSpillCount;

	// Caller-allocated home space for r3..r10 (the Win64-shadow-space
	// analog): 64 bytes on top of the fixed 48-byte header under ELFv1;
	// ZERO under ELFv2, where the parameter save area is OPTIONAL and our C
	// callees are prototyped, non-variadic and take at most 8 arguments
	// (derived from gcc's own output: an 8-argument indirect call still
	// builds only a 32-byte frame, vm/jit/ppc64/DESIGN-elfv2.md item 2).
	int paramSaveArea;

	// The stdu frame generateCCall builds around a C call from JIT code:
	// header + paramSaveArea (112 = 48 + 64 under ELFv1, 32 under ELFv2).
	int cCallFrameSize;

	// Entry stub (C -> Smalltalk) register save/restore. Contract: the save
	// hook lowers r1 by exactly entrySavedRegsSize (stdu frame: back chain,
	// CR/LR in the caller header slots, TOC, r14-r31, f14-f31) and the
	// restore hook raises it back and reloads LR/CR.
	int entrySavedRegsSize;
	void (*emitEntrySaveRegs)(AssemblerBuffer *buffer);
	void (*emitEntryRestoreRegs)(AssemblerBuffer *buffer);

	// Emit "dst = thread-pointer + tpoff". On ppc64 the thread pointer is
	// r13; tpoff is computed at RUNTIME from r13 (Thread.c), so the TLS
	// ABI's 0x7000 bias cancels out and this is a plain addis/addi ha/lo
	// sum, one fixed 2-instruction shape for any 32-bit tpoff.
	void (*emitLoadTls)(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff);

	// Emit a call to an absolute C function. ELFv1: `cFunction` points at an
	// .opd function DESCRIPTOR {entry, TOC, environ}, the emitted code
	// saves the caller's r2 to its ABI slot, loads the real entry and the
	// callee TOC from the descriptor, calls via CTR, and restores r2.
	// Clobbers r0, r11, CTR, LR (and r2 across the call). Precondition: r1
	// holds a valid ABI frame (the TOC save slot belongs to it).
	void (*emitCallCFunction)(AssemblerBuffer *buffer, intptr_t cFunction);

	// The CCALL-primitive trampoline BODY: marshal the Smalltalk args, call
	// cFunction, decode the PrimitiveResult, branch to failLabel when the
	// primitive failed (leave r3 = value otherwise). One whole-body hook
	// rather than x64's args/check pair because the two conventions differ
	// STRUCTURALLY, not just in constants: ELFv2 gets the result back in
	// r3:r4 with unshifted args and can let generateCCall own the frame;
	// ELFv1 returns through a HIDDEN sret pointer in r3 (every argument
	// shifts right by one, the Win64-class silent break, see PORTING.md) and
	// must read the buffer back BEFORE the frame teardown, so it builds the
	// fused frame itself. The shared shell in PrimitivesPpc64.c owns only
	// the blr and the fallthrough-to-fallback tail.
	// The buffer is passed alongside the OPAQUE generator: instance TUs must
	// not include jit/CodeGenerator.h (on a foreign golden host it would drag
	// in the HOST backend's assembler), and only the ELFv2 body needs the
	// generator at all, to drive generateCCall.
	void (*emitCCallPrimitive)(AssemblerBuffer *buffer,
		struct CodeGenerator *generator, intptr_t cFunction, size_t argsSize,
		AssemblerLabel *failLabel);

	// Stackful-coroutine pair under per-ABI UNIQUE symbols. Production binds
	// the jit/TargetFiber.h names by direct-call wrappers in Bind.c; the
	// prime half is pure C (host-independent, golden-testable), the switch
	// half is real POWER asm (FiberElfV1.c, ppc64 builds only, foreign
	// golden hosts link a FAIL() stub).
	void (*fiberSwitch)(void **saveSp, void *newSp);
	void *(*fiberPrimeStack)(void *top, void (*entry)(void));
} Ppc64Abi;

extern const Ppc64Abi AbiPpc64ElfV1;
extern const Ppc64Abi AbiPpc64ElfV2;

// Bound by the selected abi/<abi>/Abi<Abi>Bind.c, which exists only in real
// ppc64 builds. Host-compiled golden code must use &AbiPpc64ElfV1 or
// &AbiPpc64ElfV2 directly.
extern const Ppc64Abi *const gPpc64Abi;

// Shared table-driven spill loops (same names/contract as the x64 Abi.h ,
// no TU ever includes both): ONE copy of the order logic for every
// slow-path C call that must preserve the JIT's live caller-saved set.
// POWER has no push/pop instructions; asmPush/asmPop are the stdu/ld pair.
static inline void abiEmitCallerSavedPush(const Ppc64Abi *abi, AssemblerBuffer *buffer)
{
	for (uint8_t i = 0; i < abi->callerSavedSpillCount; i++) {
		asmPush(buffer, abi->callerSavedSpill[i]);
	}
}

static inline void abiEmitCallerSavedPop(const Ppc64Abi *abi, AssemblerBuffer *buffer)
{
	for (uint8_t i = abi->callerSavedSpillCount; i-- > 0; ) {
		asmPop(buffer, abi->callerSavedSpill[i]);
	}
}

#endif
