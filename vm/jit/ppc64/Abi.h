#ifndef PPC64_ABI_H
#define PPC64_ABI_H

// The ppc64 (big-endian) platform-ABI binding, mirroring vm/jit/x64/Abi.h:
// everything the JIT emits or executes that depends on the C calling
// convention lives behind one ops-struct. For this backend the only ABI is
// ELFv1 (AIX heritage: function descriptors, TOC save at 40(r1), 48-byte
// frame header + caller-allocated parameter save area); ppc64le/ELFv2 is a
// SEPARATE backend with its own struct.
//
// NOT parameterized here, by design (same rule as x64): the VM-INTERNAL
// calling convention (Smalltalk stack, TMP/CTX roles, allocation pool) —
// those are backend facts, not ABI facts.
//
// Instances export ONLY abi-suffixed symbols (AbiPpc64ElfV1,
// fiberSwitchElfV1, ...) and the instance TU is HOST-INDEPENDENT (pure byte
// emitters + the C stack-priming), so a foreign build host can link it and
// golden-test every hook natively — see vm/tests/EmitGoldenPpc64.c. The
// generic names (gPpc64Abi, asmLoadTls, fiberSwitchAsm,
// fiberTargetPrimeStack) live exclusively in the selected ABI's
// Abi<Abi>Bind.c, which only a real ppc64 build links.
#include "jit/ppc64/AssemblerPpc64.h"

typedef struct Ppc64Abi {
	const char *name;                    // "elfv1"

	// C integer-argument registers, true C-ABI order: r3..r10.
	const uint8_t *argRegs;
	uint8_t argRegsCount;

	// Callee-saved map indexed by Register 0..31 (r1 SP, r2 TOC and r13 TP
	// are RESERVED and marked saved; the JIT must never allocate them).
	const _Bool *calleeSaved;

	// Registers the JIT may hold live values in that this C ABI clobbers,
	// spilled around slow-path C calls. ORDER IS CONTRACT (golden-pinned).
	// PLACEHOLDER until codegen lands: pool volatiles + r3-r6 (result/send
	// scratch, mirroring x64's RAX/RSI/RDI extras).
	const uint8_t *callerSavedSpill;
	uint8_t callerSavedSpillCount;

	// Caller-allocated home space for r3..r10 required by the ELFv1 stack
	// frame (the Win64-shadow-space analog, 64 bytes), on top of the fixed
	// 48-byte frame header. Consumed when the ppc64 generateCCall lands.
	int paramSaveArea;

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
	// sum — one fixed 2-instruction shape for any 32-bit tpoff.
	void (*emitLoadTls)(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff);

	// Emit a call to an absolute C function. ELFv1: `cFunction` points at an
	// .opd function DESCRIPTOR {entry, TOC, environ} — the emitted code
	// saves the caller's r2 to its ABI slot, loads the real entry and the
	// callee TOC from the descriptor, calls via CTR, and restores r2.
	// Clobbers r0, r11, CTR, LR (and r2 across the call). Precondition: r1
	// holds a valid ABI frame (the TOC save slot belongs to it).
	void (*emitCallCFunction)(AssemblerBuffer *buffer, intptr_t cFunction);

	// CCALL-primitive marshalling + PrimitiveResult decode.
	// PORT_ME(elfv1-sret): ELFv1 returns the 16-byte PrimitiveResult through
	// a HIDDEN pointer in r3 (every argument shifts right by one) — the
	// Win64-class silent break, see PORTING.md. FAIL()-stubs until the ppc64
	// generateCCall frame design exists.
	void (*emitCCallPrimArgs)(AssemblerBuffer *buffer, size_t argsSize);
	void (*emitPrimResultCheck)(AssemblerBuffer *buffer, AssemblerLabel *failLabel);

	// Stackful-coroutine pair under per-ABI UNIQUE symbols. Production binds
	// the jit/TargetFiber.h names by direct-call wrappers in Bind.c; the
	// prime half is pure C (host-independent, golden-testable), the switch
	// half is real POWER asm (FiberElfV1.c, ppc64 builds only — foreign
	// golden hosts link a FAIL() stub).
	void (*fiberSwitch)(void **saveSp, void *newSp);
	void *(*fiberPrimeStack)(void *top, void (*entry)(void));
} Ppc64Abi;

extern const Ppc64Abi AbiPpc64ElfV1;

// Bound by abi/elfv1/AbiElfV1Bind.c — exists only in real ppc64 builds.
// Host-compiled golden code must use &AbiPpc64ElfV1 directly.
extern const Ppc64Abi *const gPpc64Abi;

// Shared table-driven spill loops (same names/contract as the x64 Abi.h —
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
