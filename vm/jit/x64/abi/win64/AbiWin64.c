// x86-64 Microsoft x64: Windows.
//
// Four differences from System V, and every one of them is a table entry here
// rather than a branch in the emitter:
//
//   * FOUR argument registers, not six, and different ones;
//   * 32 bytes of SHADOW SPACE the caller must reserve for the callee to spill
//     its register arguments into, even when the callee does not;
//   * RSI and RDI are callee-saved, so the allocator may not treat them as
//     scratch across a call;
//   * no red zone at all.
//
// Not yet exercised by a running Windows build, and written down anyway so
// that port starts from a table instead of from a hunt.

#include "jit/x64/EmitterX64.h"
#include "jit/x64/AssemblerX64.h"

static const uint8_t gArguments[] = { RCX, RDX, R8, R9 };
static const uint8_t gCalleeSaved[] = { RBX, RBP, RDI, RSI, R12, R13, R14, R15 };
static const uint8_t gCallerSaved[] = { RAX, RCX, RDX, R8, R9, R10, R11 };
static const uint8_t gAllocatable[] = { RBX, RDX, RSI, RDI, R8, R9, R10, R11,
	R12, R13, R14, R15 };

const Abi gAbiX64Win64 = {
	.name = "x64-win64",
	.argumentRegisters = gArguments,
	.argumentRegisterCount = 4,
	.integerResult = RAX,
	.calleeSaved = gCalleeSaved,
	.calleeSavedCount = 8,
	.callerSaved = gCallerSaved,
	.callerSavedCount = 7,
	.allocatableInteger = gAllocatable,
	.allocatableIntegerCount = 12,
	.stackAlignment = 16,
	.shadowSpaceBytes = 32,
	.redZoneBytes = 0,
};

const MacroAssemblerOps gMacroAssemblerX64Win64 = {
	.name = "x64-win64",
	.abi = &gAbiX64Win64,
	X64_EMITTER_OPS,
};
