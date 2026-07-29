// x86-64 System V: Linux, the BSDs, macOS.
//
// The emitter lives in vm/jit/x64/; this file is only the ABI's answers to
// which registers carry what.

#include "jit/x64/EmitterX64.h"
#include "jit/x64/AssemblerX64.h"

static const uint8_t gArguments[] = { RDI, RSI, RDX, RCX, R8, R9 };
static const uint8_t gCalleeSaved[] = { RBX, RBP, R12, R13, R14, R15 };
static const uint8_t gCallerSaved[] = { RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11 };
// RAX and RCX are the macro assembler's own scratch and RBP/RSP are the frame,
// so none of them is offered to the SSA backend's register allocator.
static const uint8_t gAllocatable[] = { RBX, RDX, RSI, RDI, R8, R9, R10, R11,
	R12, R13, R14, R15 };

// The SSE file, in its OWN numbering (jit/x64/AssemblerX64.h): these are XMM
// numbers, not the general-register numbers above, and nothing outside the x64
// backend interprets either.
//
// XMM0 is held back as the emitter's scratch, for the same reason RAX and RCX
// are held back from the integer set: a sequence that has to stage a value --
// a negation through the integer file, a spilled operand reloaded for one
// instruction -- needs a register the allocator cannot have given away.
//
// EVERY SSE REGISTER IS CALLER-SAVED under SysV, which is not a detail the
// allocator can infer: it reads calleeSaved to decide what survives a call, and
// an empty float half of that list is the correct answer here rather than an
// unfinished one. On Win64 XMM6 to XMM15 are callee-saved, which is why this is
// a table and not a constant.
static const uint8_t gAllocatableFloat[] = { XMM1, XMM2, XMM3, XMM4, XMM5,
	XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15 };
static const uint8_t gFloatArguments[] = { XMM0, XMM1, XMM2, XMM3, XMM4, XMM5,
	XMM6, XMM7 };

const Abi gAbiX64SysV = {
	.name = "x64-sysv",
	.argumentRegisters = gArguments,
	.argumentRegisterCount = 6,
	.integerResult = RAX,
	.calleeSaved = gCalleeSaved,
	.calleeSavedCount = 6,
	.callerSaved = gCallerSaved,
	.callerSavedCount = 9,
	.allocatableInteger = gAllocatable,
	.allocatableIntegerCount = 12,
	.floatArgumentRegisters = gFloatArguments,
	.floatArgumentRegisterCount = 8,
	.floatResult = XMM0,
	.allocatableFloat = gAllocatableFloat,
	.allocatableFloatCount = 15,
	.stackAlignment = 16,
	.shadowSpaceBytes = 0,
	// 128 bytes below the stack pointer that a signal handler will not
	// disturb. A leaf function may use it without adjusting the stack pointer;
	// Win64 and AIX have none, which is why this is a field and not a constant.
	.redZoneBytes = 128,
};

const MacroAssemblerOps gMacroAssemblerX64SysV = {
	.name = "x64",
	.abi = &gAbiX64SysV,
	X64_EMITTER_OPS,
};
