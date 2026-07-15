#ifndef X64_ABI_H
#define X64_ABI_H

// The x86-64 platform-ABI binding: everything the JIT emits or executes that
// depends on the C calling convention / platform ABI (SysV vs Win64), behind
// one ops-struct. Every member is consumed at JIT-EMIT time except the two
// fiber pointers (and even those are bound by direct-call wrappers in the
// selected ABI's Bind.c) — generated code pays zero indirection cost.
//
// NOT parameterized here, by design: the VM-INTERNAL calling convention
// (args/receiver on the Smalltalk stack, native-code target in R11, result in
// RAX, CTX=R12, TMP=R10, push-rbp frames, the register-allocation pool) is
// IDENTICAL under SysV and Win64 because the registers the JIT keeps live
// across C calls — {RBX, R12..R15} — are callee-saved in both. Do not let a
// future ABI instance diverge it.
//
// Instances export ONLY abi-suffixed symbols (AbiX64SysV, fiberSwitchSysV...)
// so several instances can link into one test binary for golden-byte emission
// tests of a foreign ABI (see vm/tests/EmitGoldenX64.c). All generic names
// (gX64Abi, fiberSwitchAsm, fiberTargetPrimeStack) live exclusively in the
// selected ABI's Abi<Abi>Bind.c — CMake's ST_ABI picks exactly one.
#include "jit/x64/AssemblerX64.h"

typedef struct X64Abi {
	const char *name;                    // "sysv" (future: "win64")

	// C integer-argument registers, true C-ABI order, no sret adjustment
	// (sysv: RDI,RSI,RDX,RCX,R8,R9 / win64: RCX,RDX,R8,R9). Consumed by the
	// entry stub's in-arg binding and this ABI's own emitCCallPrimArgs.
	const uint8_t *argRegs;
	uint8_t argRegsCount;

	// Callee-saved map indexed by Register 0..15. Kept as data so the golden
	// self-test can assert the cross-member invariants that keep an ABI
	// instance honest (spill list == clobberable live set, argRegs volatile).
	const _Bool *calleeSaved;

	// Registers the JIT may hold live values in that this C ABI clobbers,
	// spilled around slow-path C calls (safepoint poll, store-check grow).
	// ORDER IS CONTRACT: pushed in index order, popped in reverse — golden
	// byte-compatibility depends on it.
	const uint8_t *callerSavedSpill;
	uint8_t callerSavedSpillCount;       // sysv: 8 {RAX,RCX,RDX,RSI,RDI,R8,R9,R11}

	// Callee-owned scratch the C callee may write at [rsp .. rsp+shadowSpace).
	// generateCCall subtracts it after the 16-byte alignment (32 keeps the
	// alignment); no matching add — RSP is restored from RBP. sysv: 0, win64: 32.
	int shadowSpace;

	// Entry stub (C -> Smalltalk): byte size of the register-save area plus
	// the emitters that produce/undo it. Contract: emitEntrySaveRegs lowers
	// RSP by exactly entrySavedRegsSize; the EntryStackFrame layout and the
	// rest of the stub are VM-internal and shared. (Win64 fits: GP pushes +
	// sub rsp + movdqa xmm6-15 all inside the hook.)
	int entrySavedRegsSize;              // sysv: 48 (6 pushes)
	void (*emitEntrySaveRegs)(AssemblerBuffer *buffer);
	void (*emitEntryRestoreRegs)(AssemblerBuffer *buffer);

	// Emit "dst = thread-pointer + tpoff" (the shared-JIT-code route to the
	// RUNNING worker's TLS): sysv = %fs:0 self-ref + lea; win64 = %gs TEB —
	// a different mechanism entirely, see PORTING.md.
	void (*emitLoadTls)(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff);

	// CCALL-primitive marshalling: load argsSize Smalltalk-stack arguments
	// (slot i at [RSP + (i+1)*8] — VM frame layout) into this ABI's C argument
	// positions. Win64 additionally materializes the hidden PrimitiveResult
	// sret pointer here (real args shift right by one).
	void (*emitCCallPrimArgs)(AssemblerBuffer *buffer, size_t argsSize);

	// Decode the just-returned PrimitiveResult (the two-word struct in
	// runtime/Primitives.h — kept as-is across ABIs): leave value in RAX and
	// branch to failLabel iff failed. Runs after generateCCall restored
	// RSP/RBP/CTX. sysv: RAX:RDX two-register return -> test rdx,rdx; jnz.
	// win64: reads the sret buffer its own emitCCallPrimArgs reserved
	// (intra-ABI contract — both hooks live in the same file).
	void (*emitPrimResultCheck)(AssemblerBuffer *buffer, AssemblerLabel *failLabel);

	// Stackful-coroutine pair under per-ABI UNIQUE symbols. Production binds
	// the jit/TargetFiber.h names by direct-call wrappers in Bind.c (no
	// indirection on the switch path); these pointers exist so tests can
	// exercise a foreign ABI's stack priming/switch layout.
	void (*fiberSwitch)(void **saveSp, void *newSp);
	void *(*fiberPrimeStack)(void *top, void (*entry)(void));
} X64Abi;

extern const X64Abi AbiX64SysV;
extern const X64Abi *const gX64Abi;   // bound by the selected abi/<abi>/Abi<Abi>Bind.c

// Shared table-driven spill loops: ONE copy of the order logic for every
// slow-path C call that must preserve the JIT's live caller-saved set.
// Explicit abi parameter so golden tests can drive a foreign instance;
// production passes gX64Abi.
static inline void abiEmitCallerSavedPush(const X64Abi *abi, AssemblerBuffer *buffer)
{
	for (uint8_t i = 0; i < abi->callerSavedSpillCount; i++) {
		asmPushq(buffer, abi->callerSavedSpill[i]);
	}
}

static inline void abiEmitCallerSavedPop(const X64Abi *abi, AssemblerBuffer *buffer)
{
	for (uint8_t i = abi->callerSavedSpillCount; i-- > 0; ) {
		asmPopq(buffer, abi->callerSavedSpill[i]);
	}
}

#endif
