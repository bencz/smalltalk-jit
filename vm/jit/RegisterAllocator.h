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
	uint16_t index; // 16-bit: variable indexes follow the 16-bit operand index space
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

// Context levels are a block-nesting depth (a uint8 in the operand encoding),
// so that specialVars row keeps a fixed size; the VAR_ASSOC row is indexed by
// LITERAL index (16-bit) and is sized per-method from its literal count.
#define REGS_ALLOC_CONTEXT_LEVELS 256

// All arrays are heap-allocated per compilation (computeRegsAlloc computes the
// real capacity from the method: args + temps + specials); the old fixed
// vars[256]/specialVars[2][256] capped a method at ~256 variables and silently
// aliased associations whose literal index exceeded 255.
typedef struct {
	AvailableRegs *regs;
	size_t varsSize;
	size_t varsCapacity;
	Variable *vars;
	Variable **specialVars[2]; // rows: VAR_CONTEXT (fixed levels), VAR_ASSOC (per-literal)
	size_t assocCapacity;      // entries in the VAR_ASSOC row
	size_t frameSize;
	_Bool frameLess;
} RegsAlloc;

void computeRegsAlloc(RegsAlloc *alloc, AvailableRegs *regs, CompiledCode *code);
void invalidateRegs(RegsAlloc *alloc);
// Grow (never shrink) the alloc's arrays to at least the given capacities.
// Also the pre-computeRegsAlloc hook for the primitive prologue, which touches
// vars[0] before the real capacities are known.
void regsAllocEnsure(RegsAlloc *alloc, size_t varsCapacity, size_t assocCapacity);
void regsAllocFree(RegsAlloc *alloc);

#endif
