// The System V AMD64 ABI instance (Linux; macOS shares the call convention
// but NOT the %fs TLS mechanism — see PORTING.md before reusing this file for
// a darwin port). Exports ONLY sysv-suffixed symbols so a foreign-ABI test
// binary can link several instances side by side; the generic bindings live
// in AbiSysVBind.c.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/Abi.h"
#include "jit/x64/abi/sysv/FiberSysV.h"
#include "core/Assert.h"

// C integer-argument registers, SysV order.
static const uint8_t SysVArgRegs[] = { RDI, RSI, RDX, RCX, R8, R9 };

// SysV callee-saved map, indexed by Register 0..15.
static const _Bool SysVCalleeSaved[16] = {
	/* RAX */ 0, /* RCX */ 0, /* RDX */ 0, /* RBX */ 1,
	/* RSP */ 1, /* RBP */ 1, /* RSI */ 0, /* RDI */ 0,
	/* R8  */ 0, /* R9  */ 0, /* R10 */ 0, /* R11 */ 0,
	/* R12 */ 1, /* R13 */ 1, /* R14 */ 1, /* R15 */ 1,
};

// Registers the JIT may hold live values in that SysV clobbers across a C
// call, spilled around slow-path calls. ORDER IS CONTRACT (golden-pinned):
// pushed in this order, popped in reverse.
static const uint8_t SysVCallerSavedSpill[] = { RAX, RCX, RDX, RSI, RDI, R8, R9, R11 };

// mov dst, %fs:[disp32] — segment-prefixed absolute load from the %fs
// (thread-pointer) segment. Encoding:
//   64 (FS prefix)  REX.W[.R]  8B  ModRM(mod=00,reg=dst,rm=100=SIB)  SIB(0x25)  disp32
static void sysvMovqFsAbs(AssemblerBuffer *buffer, int32_t disp, Register dst)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, 0x64);                             // FS segment override prefix
	asmEmitRex(buffer, REX_W | (uint8_t) ((dst & 8) >> 1)); // REX.W (+ REX.R for R8..R15 in reg field)
	asmEmitUint8(buffer, 0x8B);                             // MOV r64, r/m64
	asmEmitUint8(buffer, (uint8_t) (((dst & 7) << 3) | 4)); // mod=00, reg=dst, rm=100 (SIB follows)
	asmEmitUint8(buffer, 0x25);                             // SIB: scale=0, index=none(100), base=disp32(101)
	asmEmitInt32(buffer, disp);
}

// Load the address of the running worker's initial-exec TLS variable into
// `dst`: dst = threadPointer (%fs:0 self-reference) + tpoff. `tpoff` is a
// link-time constant, the SAME on every thread — so JIT code compiled once
// and shared by every worker still reaches EACH worker's own TLS block.
static void sysvEmitLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	sysvMovqFsAbs(buffer, 0, dst);                               // dst = thread pointer
	asmLeaq(buffer, asmMem(dst, NO_REGISTER, SS_1, tpoff), dst); // dst += tpoff
}

// Load the CCALL primitive's Smalltalk-stack arguments (receiver+args pushed
// by the send: slot i at [RSP + (i+1)*8]) into the SysV argument registers.
static void sysvEmitCCallPrimArgs(AssemblerBuffer *buffer, size_t argsSize)
{
	ASSERT(argsSize <= sizeof(SysVArgRegs));
	for (size_t i = 0; i < argsSize; i++) {
		asmMovqMem(buffer, asmMem(RSP, NO_REGISTER, SS_1, (i + 1) * sizeof(intptr_t)),
			SysVArgRegs[i]);
	}
}

// SysV returns the 16-byte PrimitiveResult in RAX:RDX = value:failed. The
// value is already where the VM wants it (RAX); test the flag and branch.
static void sysvEmitPrimResultCheck(AssemblerBuffer *buffer, AssemblerLabel *failLabel)
{
	asmTestq(buffer, RDX, RDX);
	asmJ(buffer, COND_NOT_ZERO, failLabel);
}

// Entry-stub register save/restore: the SysV callee-saved GP set. Must lower
// RSP by exactly AbiX64SysV.entrySavedRegsSize (6 * 8 = 48).
static void sysvEmitEntrySaveRegs(AssemblerBuffer *buffer)
{
	asmPushq(buffer, RBP);
	asmPushq(buffer, RBX);
	asmPushq(buffer, R12);
	asmPushq(buffer, R13);
	asmPushq(buffer, R14);
	asmPushq(buffer, R15);
}

static void sysvEmitEntryRestoreRegs(AssemblerBuffer *buffer)
{
	asmPopq(buffer, R15);
	asmPopq(buffer, R14);
	asmPopq(buffer, R13);
	asmPopq(buffer, R12);
	asmPopq(buffer, RBX);
	asmPopq(buffer, RBP);
}

const X64Abi AbiX64SysV = {
	.name = "sysv",
	.argRegs = SysVArgRegs,
	.argRegsCount = sizeof(SysVArgRegs),
	.calleeSaved = SysVCalleeSaved,
	.callerSavedSpill = SysVCallerSavedSpill,
	.callerSavedSpillCount = sizeof(SysVCallerSavedSpill),
	.shadowSpace = 0,
	.entrySavedRegsSize = 6 * sizeof(intptr_t),
	.emitEntrySaveRegs = sysvEmitEntrySaveRegs,
	.emitEntryRestoreRegs = sysvEmitEntryRestoreRegs,
	.emitLoadTls = sysvEmitLoadTls,
	.emitCCallPrimArgs = sysvEmitCCallPrimArgs,
	.emitPrimResultCheck = sysvEmitPrimResultCheck,
	.fiberSwitch = fiberSwitchSysV,
	.fiberPrimeStack = fiberPrimeStackSysV,
};
