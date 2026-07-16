#ifndef REGISTER_ALLOCATOR_H
#define REGISTER_ALLOCATOR_H

#include "core/CompiledCode.h"
#include "compiler/Bytecodes.h"
#include "jit/Assembler.h"

#define SPILLED_REG -1

// Only VAR_CONTEXT and VAR_ASSOC index specialVars[], so it is sized for those two.
// VAR_TMP is NOT a specialVars row: temps are addressed by their own operand index in
// vars[]. (A VAR_CLASS shadow used to sit at 1, reserving a register per send receiver
// for a class cache the backends no longer keep; see examineOperandClass's note in
// RegisterAllocator.c.)
typedef enum {
	VAR_CONTEXT = 0,
	VAR_ASSOC = 1,
	VAR_TMP = 2,
} VariableType;

typedef enum {
	VAR_DEFINED = 1,
	VAR_IN_REG = 1 << 1,
	VAR_ON_STACK = 1 << 2,
} VariableFlags;

typedef struct {
	uint8_t index;
	uint8_t type;
	VariableFlags flags;
	int8_t reg;
	size_t start;
	size_t end;
	// GC liveness bound, >= end. `end` is the last TEXTUAL use and drives register
	// allocation only. A variable inside a loop is live around the back-edge though
	// its last textual use may precede it, so the stackmap filter uses gcEnd, which
	// extendLoopVarRanges widens to each enclosing loop's back-edge. Keeping the two
	// apart adds GC coverage without changing register pressure/assignment at all.
	size_t gcEnd;
	ptrdiff_t frameOffset;
} Variable;

typedef struct {
	AvailableRegs *regs;
	uint8_t varsSize;
	Variable vars[256];
	Variable *specialVars[2][256]; // rows: VAR_CONTEXT, VAR_ASSOC
	size_t frameSize;
	_Bool frameLess;
} RegsAlloc;

void computeRegsAlloc(RegsAlloc *alloc, AvailableRegs *regs, CompiledCode *code);
void invalidateRegs(RegsAlloc *alloc);

#endif
