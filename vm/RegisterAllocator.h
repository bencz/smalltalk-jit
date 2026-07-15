#ifndef REGISTER_ALLOCATOR_H
#define REGISTER_ALLOCATOR_H

#include "CompiledCode.h"
#include "Bytecodes.h"
#include "Assembler.h"

#define SPILLED_REG -1

typedef enum {
	VAR_CONTEXT = 0,
	VAR_CLASS = 1,
	VAR_ASSOC = 2,
	VAR_TMP = 3,
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
	Variable *specialVars[3][256];
	size_t frameSize;
	_Bool frameLess;
} RegsAlloc;

void computeRegsAlloc(RegsAlloc *alloc, AvailableRegs *regs, CompiledCode *code);
void invalidateRegs(RegsAlloc *alloc);

#endif
