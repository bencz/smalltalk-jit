#ifndef PPC64LE_ABI_H
#define PPC64LE_ABI_H

// The ppc64le (little-endian) platform-ABI binding, mirroring vm/jit/x64/Abi.h
// and vm/jit/ppc64/Abi.h: everything the JIT emits or executes that depends on
// the C calling convention lives behind one ops-struct. For this backend the
// only ABI is ELFv2 (no function descriptors, TOC save at 24(r1), 32-byte frame
// header, no mandatory parameter save area, small aggregates returned in
// r3:r4); big-endian ppc64/ELFv1 is a SEPARATE backend with its own struct.
//
// NOT parameterized here, by design (same rule as x64): the VM-INTERNAL
// calling convention (Smalltalk stack, TMP/CTX roles, allocation pool). Those
// are backend facts, not ABI facts, and they are IDENTICAL to the ppc64
// backend's (vm/jit/ppc64/DESIGN.md pins them).
//
// Instances export ONLY abi-suffixed symbols (AbiPpc64leElfV2,
// fiberSwitchElfV2, ...) and the instance TU is HOST-INDEPENDENT (pure byte
// emitters plus the C stack-priming), so a foreign build host can link it and
// golden-test every hook natively: see vm/tests/EmitGoldenPpc64le.c. The
// generic names (gPpc64leAbi, asmLoadTls, fiberSwitchAsm,
// fiberTargetPrimeStack) live exclusively in the selected ABI's
// Abi<Abi>Bind.c, which only a real ppc64le build links.
#include "jit/ppc64le/AssemblerPpc64le.h"

typedef struct Ppc64leAbi {
	const char *name;                    // "elfv2"

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

	// Caller-allocated home space for the argument registers. ZERO under
	// ELFv2: the parameter save area is OPTIONAL and our C callees are
	// prototyped, non-variadic and take at most 8 arguments. Derived from
	// gcc's own output, not from the spec: see vm/jit/ppc64le/DESIGN.md
	// item 2 (an 8-argument indirect call still builds only a 32-byte frame).
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
	// sum, one fixed 2-instruction shape for any 32-bit tpoff. Identical to
	// ELFv1: TLS is a generic-POWER fact, not an ABI-version one.
	void (*emitLoadTls)(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff);

	// Emit a call to an absolute C function. ELFv2 has no descriptors: the
	// target address goes in r12 (the ABI requires it, so the callee's
	// global-entry prologue can derive its own r2 from it), then mtctr/bctrl
	// with the caller's TOC saved around the call.
	// WARNING: clobbers r12 (== the VM's TGT), CTR, LR and r2 across the
	// call. The ELFv1 hook clobbers r11 (TMP) instead, so every call site
	// copied from the BE backend must be re-checked for a live TGT.
	// Precondition: r1 holds a valid ABI frame (the TOC save slot is in it).
	void (*emitCallCFunction)(AssemblerBuffer *buffer, intptr_t cFunction);

	// CCALL-primitive marshalling plus PrimitiveResult decode. REAL on this
	// backend (the ELFv1 pair FAIL()s): ELFv2 returns the 16-byte
	// PrimitiveResult in r3:r4 with arguments UNSHIFTED, exactly the SysV/x64
	// shape, so there is no hidden-sret hazard here at all.
	void (*emitCCallPrimArgs)(AssemblerBuffer *buffer, size_t argsSize);
	void (*emitPrimResultCheck)(AssemblerBuffer *buffer, AssemblerLabel *failLabel);

	// Stackful-coroutine pair under per-ABI UNIQUE symbols. Production binds
	// the jit/TargetFiber.h names by direct-call wrappers in Bind.c; the
	// prime half is pure C (host-independent, golden-testable), the switch
	// half is real POWER asm (FiberElfV2.c, ppc64le builds only; foreign
	// golden hosts link a FAIL() stub).
	void (*fiberSwitch)(void **saveSp, void *newSp);
	void *(*fiberPrimeStack)(void *top, void (*entry)(void));
} Ppc64leAbi;

extern const Ppc64leAbi AbiPpc64leElfV2;

// Bound by abi/elfv2/AbiElfV2Bind.c: exists only in real ppc64le builds.
// Host-compiled golden code must use &AbiPpc64leElfV2 directly.
extern const Ppc64leAbi *const gPpc64leAbi;

// Shared table-driven spill loops (same names/contract as the x64 and ppc64
// Abi.h: no TU ever includes two of them, and AssemblerPpc64le.h enforces the
// POWER half of that with a hard #error). ONE copy of the order logic for
// every slow-path C call that must preserve the JIT's live caller-saved set.
// POWER has no push/pop instructions; asmPush/asmPop are the stdu/ld pair.
static inline void abiEmitCallerSavedPush(const Ppc64leAbi *abi, AssemblerBuffer *buffer)
{
	for (uint8_t i = 0; i < abi->callerSavedSpillCount; i++) {
		asmPush(buffer, abi->callerSavedSpill[i]);
	}
}

static inline void abiEmitCallerSavedPop(const Ppc64leAbi *abi, AssemblerBuffer *buffer)
{
	for (uint8_t i = abi->callerSavedSpillCount; i-- > 0; ) {
		asmPop(buffer, abi->callerSavedSpill[i]);
	}
}

#endif
