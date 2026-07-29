// Gate level 4: bytecode becomes SSA, with deoptimization state attached.
//
// The two things worth proving here are the ones that are easy to get subtly
// wrong and impossible to notice later:
//
//   * a PHI appears exactly where control merges and a register was defined
//     differently on each path, including at a LOOP HEADER, where the second
//     operand comes from a back edge that has not been walked yet. That is the
//     case Braun's incomplete phis exist for, and the case a naive builder
//     silently gets wrong by reading a definition that does not dominate;
//   * a deoptimization state names only the registers LIVE at its bytecode
//     index. If it named all of them the states would be large and, much worse,
//     every value they mention would be kept alive and dead-code elimination
//     would stop eliminating anything.

#include "compiler/Bytecode.h"
#include "jit/Ir.h"
#include "jit/SsaBuild.h"
#include "runtime/Closure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFailures;
static int gChecks;

static void check(const char *what, int ok)
{
	gChecks++;
	if (!ok) {
		gFailures++;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  ok    %s\n", what);
	}
}


static CodeUnit *makeUnit(Instruction *code, uint16_t count, uint16_t registers,
	uint16_t arguments)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->code = code;
	unit->instructionCount = count;
	unit->registerCount = registers;
	unit->argumentCount = arguments;
	return unit;
}


static uint32_t countOp(IrFunction *function, IrOp op)
{
	uint32_t count = 0;
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			count += value->op == op;
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			count += value->op == op;
		}
		if (block->terminator != NULL) {
			count += block->terminator->op == op;
		}
	}
	return count;
}


static IrValue *findOp(IrFunction *function, IrOp op)
{
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			if (value->op == op) {
				return value;
			}
		}
	}
	return NULL;
}


int main(void)
{
	printf("gate level 4: bytecode becomes SSA, with deopt state attached\n\n");

	// ---- straight line -----------------------------------------------------
	static Instruction straight[] = {
		{ OP_LOADI, 0, 1, 42, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	IrFunction *simple = ssaBuild(makeUnit(straight, 2, 2, 0), NULL);
	check("straight-line code is one block", simple->blockCount == 1);
	check("the constant becomes a value", countOp(simple, IR_CONST) == 1);
	check("the return is a terminator", countOp(simple, IR_RET) == 1);
	check("no phi where control never merges", countOp(simple, IR_PHI) == 0);

	// ---- a merge, which is where a phi belongs ----------------------------
	static Instruction merge[] = {
		{ OP_JUMPFALSE, 0, 1, 3, 0 },
		{ OP_LOADI, 0, 2, 100, 0 },
		{ OP_JUMP, 0, 4, 0, 0 },
		{ OP_LOADI, 0, 2, 200, 0 },
		{ OP_RET, 0, 2, 0, 0 },
	};
	IrFunction *merged = ssaBuild(makeUnit(merge, 5, 3, 1), NULL);
	check("a branch and a join make four blocks", merged->blockCount == 4);
	check("the merge introduces exactly one phi", countOp(merged, IR_PHI) == 1);

	IrBlock *joinBlock = NULL;
	for (IrBlock *block = merged->blocks; block != NULL; block = block->next) {
		if (block->phis != NULL) {
			joinBlock = block;
		}
	}
	check("the phi is in the block with two predecessors",
		joinBlock != NULL && joinBlock->predCount == 2);
	check("the phi has one operand per predecessor",
		joinBlock != NULL && joinBlock->phis->argCount == 2);
	check("its operands are the two definitions, not the same one",
		joinBlock != NULL && joinBlock->phis->args[0] != joinBlock->phis->args[1]);

	// ---- a LOOP, where the back edge is not yet walked when the header is --
	// r1 counts, r2 is the running value. The header reads both before the back
	// edge exists, so both need incomplete phis.
	static Instruction loop[] = {
		{ OP_LOADI, 0, 1, 0, 0 },      // 0: r1 := 0
		{ OP_LOADI, 0, 2, 1, 0 },      // 1: r2 := 1
		{ OP_SAFEPOINT, 0, 0, 0, 0 },  // 2: loop header
		{ OP_JUMPFALSE, 0, 3, 6, 0 },  // 3: exit if r3 false
		{ OP_MOVE, 0, 2, 1, 0 },       // 4: r2 := r1
		{ OP_JUMP, 0, 2, 0, 0 },       // 5: back to 2
		{ OP_RET, 0, 2, 0, 0 },        // 6
	};
	IrFunction *looping = ssaBuild(makeUnit(loop, 7, 4, 1), NULL);
	IrBlock *header = NULL;
	for (IrBlock *block = looping->blocks; block != NULL; block = block->next) {
		if (block->label == 2) {
			header = block;
		}
	}
	check("the loop header exists and has two predecessors",
		header != NULL && header->predCount == 2);
	uint32_t headerPhis = 0;
	for (IrValue *phi = header != NULL ? header->phis : NULL; phi != NULL;
			phi = phi->next) {
		headerPhis++;
	}
	check("the loop header carries a phi for the value the body changes",
		headerPhis >= 1);
	_Bool operandsComplete = 1;
	for (IrValue *phi = header != NULL ? header->phis : NULL; phi != NULL;
			phi = phi->next) {
		operandsComplete = operandsComplete && phi->argCount == header->predCount;
		for (uint16_t i = 0; i < phi->argCount; i++) {
			operandsComplete = operandsComplete && phi->args[i] != NULL;
		}
	}
	check("every loop-header phi got its back-edge operand filled in",
		operandsComplete);

	// ---- deopt state: only the live registers -----------------------------
	IrValue *safepoint = findOp(looping, IR_SAFEPOINT);
	check("the safepoint carries a deopt state",
		safepoint != NULL && safepoint->deopt != NULL);
	check("the state has one frame, resumable at this bytecode index",
		safepoint != NULL && safepoint->deopt->frameCount == 1
		&& safepoint->deopt->frames[0].bci == 2);
	check("the innermost frame re-executes rather than resuming after",
		safepoint != NULL && safepoint->deopt->frames[0].innermost);
	// r0 (self) is never read, so it must NOT be in the state. That is the
	// whole point: a state that named every register would keep every value
	// alive and stop dead-code elimination from removing anything.
	_Bool namesOnlyLive = 1;
	_Bool namesR0 = 0;
	if (safepoint != NULL) {
		DeoptFrame *frame = &safepoint->deopt->frames[0];
		for (uint16_t i = 0; i < frame->slotCount; i++) {
			namesR0 = namesR0 || frame->slotRegister[i] == 0;
			namesOnlyLive = namesOnlyLive && frame->slotValue[i] != NULL;
		}
	}
	check("every named slot has a value", namesOnlyLive);
	check("a register that is never read is NOT in the state", !namesR0);

	printf("\n");
	printf("  the loop's SSA, for the record:\n");
	irPrint(looping);

	// ---- the ADR 0008 family: cells, closures, captures --------------------
	//
	// A CELL IS AN EXPLICIT ALLOCATION and its accesses are ordinary field
	// operations. That is the whole reason ADR 0008 chose cells over contexts:
	// escape analysis erases what it can see, and a context was opaque.
	static Instruction cells[] = {
		{ OP_LOADI,   0, 1, 7, 0 },
		{ OP_NEWCELL, 0, 2, 1, 0 },  // r2 := a cell holding r1
		{ OP_GETCELL, 0, 3, 2, 0 },
		{ OP_LOADI,   0, 4, 9, 0 },
		{ OP_SETCELL, 0, 2, 4, 0 },  // r2's value := r4
		{ OP_GETCELL, 0, 5, 2, 0 },
		{ OP_RET,     0, 5, 0, 0 },
	};
	IrFunction *boxed = ssaBuild(makeUnit(cells, 7, 6, 0), NULL);
	check("a cell is an explicit allocation", countOp(boxed, IR_NEWCELL) == 1);
	check("reading one is an ordinary field read", countOp(boxed, IR_FIELD_T) == 2);
	check("writing one is an ordinary field write",
		countOp(boxed, IR_SETFIELD_T) == 1);
	IrValue *cellRead = findOp(boxed, IR_FIELD_T);
	check("at the cell's value field",
		cellRead != NULL && cellRead->extra == CELL_VALUE_FIELD);
	irDestroy(boxed);

	// A CAPTURE is a field of the closure in register 0, which is what replaced
	// the old VM's walk up a chain of contexts.
	static Instruction upvalue[] = {
		{ OP_GETUP, 0, 1, 2, 0 },
		{ OP_RET,   0, 1, 0, 0 },
	};
	IrFunction *captured = ssaBuild(makeUnit(upvalue, 2, 2, 0), NULL);
	check("a captured value is one field read", countOp(captured, IR_FIELD_T) == 1);
	IrValue *upRead = findOp(captured, IR_FIELD_T);
	check("at the capture's own index",
		upRead != NULL && upRead->extra == CLOSURE_CAPTURE_FIELD(2));
	irDestroy(captured);

	static Instruction made[] = {
		{ OP_LOADI,   0, 1, 5, 0 },
		{ OP_LOADI,   0, 2, 6, 0 },
		{ OP_CLOSURE, 2, 3, 0, 1 },  // over blocks[0], capturing r1 and r2
		{ OP_RET,     0, 3, 0, 0 },
	};
	IrFunction *closed = ssaBuild(makeUnit(made, 4, 4, 0), NULL);
	check("a closure is an allocation", countOp(closed, IR_CLOSURE) == 1);
	IrValue *closure = findOp(closed, IR_CLOSURE);
	check("with one operand per capture", closure != NULL && closure->argCount == 2);
	check("naming the block it closes over", closure != NULL && closure->extra == 0);
	irDestroy(closed);

	static Instruction nonLocal[] = {
		{ OP_LOADI,    0, 1, 3, 0 },
		{ OP_RETOUTER, 0, 1, 0, 0 },
	};
	IrFunction *outer = ssaBuild(makeUnit(nonLocal, 2, 2, 0), NULL);
	check("a non-local return is a terminator of its own",
		countOp(outer, IR_RETOUTER) == 1);
	irDestroy(outer);

	// ---- and an opcode nobody modelled is REFUSED, by name -----------------
	//
	// It used to be skipped in silence, which for a method with a block meant IR
	// with the closure operations simply missing. OP_SETUP is the opcode to test
	// it with precisely because nothing emits it: it is declared, dead, and
	// therefore never going to be modelled by accident.
	static Instruction unmodelled[] = {
		{ OP_SETUP, 0, 0, 1, 0 },
		{ OP_RET,   0, 1, 0, 0 },
	};
	Opcode refused = OP_COUNT;
	check("an unmodelled opcode is refused, not skipped",
		ssaBuild(makeUnit(unmodelled, 2, 2, 0), &refused) == NULL);
	check("and the refusal names it", refused == OP_SETUP);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
