#include "jit/RegisterAllocator.h"
#include "jit/TargetAssembler.h"
#include "compiler/Compiler.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define PRINT_ALLOCATION 0

typedef struct {
	uint8_t varsSize;
	Variable *vars;
	Variable *(* specialVars)[256];
	Variable *order[256];
	Variable **last;
	size_t maxOffset;
	_Bool frameLess;
	ptrdiff_t frameSize;
} Vars;

#define SORTED_VARS_SIZE 256

typedef struct {
	Variable *vars[SORTED_VARS_SIZE];
	Variable **first;
	Variable **last;
} SortedVars;

typedef struct {
	/*AvailableRegs *available;*/
	int8_t regs[256 * 2];
	int8_t *tmp;
	int8_t *end;
} RegsPool;

static void printAllocation(Vars *vars);
static void scanCode(Vars *vars, CompiledCode *code);
static void examineCopyOperands(Vars *vars, Operand src, Operand dst, size_t offset);
static void examineOperand(Vars *vars, Operand operand, size_t offset);
static void defineTmpVar(Vars *vars, uint8_t index, size_t offset);
static Variable *defineSpecialVar(Vars *vars, uint8_t type, uint8_t index, size_t offset);
static Variable *defineVar(Vars *vars, uint8_t type, uint8_t index, size_t offset);
static void scanRegisters(Vars *vars, AvailableRegs *regs);
static void initSortedVars(SortedVars *sortedVars);
static Variable *getFirstVar(SortedVars *sortedVars);
static void addFirstVar(SortedVars *sortedVars, Variable *var);
static void addLastVar(SortedVars *sortedVars, Variable *var);
static void removeDeadVars(SortedVars *sortedVars, RegsPool *regsPool, size_t offset);
static void reuseVarReg(RegsPool *regsPool, Variable *var);
static void removeVar(SortedVars *sortedVars, Variable **var);
static Variable *defineCtxVar(Vars *vars, uint8_t index, size_t offset);
static Variable *removeFirstVar(SortedVars *sortedVars);
static void initRegsPool(RegsPool *regsPool, AvailableRegs *regs);
static uint8_t nextReg(RegsPool *regsPool);

/*
a b c d

^        a := 1.	a
| ^      b := 2.	a b
| |   ^  c := 3.	a b c
| |   |  .			a b c
| |   v  c msg.		a b c
| v      b msg.		a b
|   ^    d := 4.	a d
|   |    .			a d
|   v    d msg.		a d
v        a msg.		a

*/


void computeRegsAlloc(RegsAlloc *alloc, AvailableRegs *regs, CompiledCode *code)
{
	ASSERT(SORTED_VARS_SIZE / 2 >= code->header.tempsSize);

	Vars vars;
	memset(&vars, 0, sizeof(vars));
	memset(alloc, 0, sizeof(*alloc));

	vars.varsSize = code->header.argsSize + code->header.tempsSize + 2;
	vars.vars = alloc->vars;
	vars.specialVars = alloc->specialVars;
	vars.last = vars.order;
	vars.maxOffset = 0;
	vars.frameLess = 1;
	vars.frameSize = code->header.argsSize;
	for (int16_t i = code->header.argsSize; i >= 0 ; i--) {
		Variable *arg = defineVar(&vars, VAR_TMP, i + 1, 0);
		arg->flags |= VAR_ON_STACK;
	}
	vars.frameSize--;
	vars.specialVars[VAR_CONTEXT][0] = defineVar(&vars, VAR_CONTEXT, CONTEXT_INDEX, 0);

	scanCode(&vars, code);
	scanRegisters(&vars, regs);

	// A frameless method has no frame AT ALL: the backends skip generatePrologue, so
	// RBP is never established and no spill area is reserved (or nil-initialised).
	// scanCode decides frameLess from the bytecodes alone, but scanRegisters runs
	// AFTER it and may spill -- and a spilled variable has nowhere to live. Every
	// spill accessor except fillVar also addresses the slot off the frame pointer,
	// which in a frameless method is still the CALLER's, so a spill there silently
	// wrote through the caller's frame (losing the store, and corrupting the saved
	// frame pointer and return address).
	//
	// So the two decisions are not independent: spilling REQUIRES a frame. Any method
	// with more live variables than the register pool must be framed. This bites only
	// methods that both are frameless (no send, block, context var or outer return)
	// and outgrow the pool -- e.g. a pure setter with 10+ arguments -- so ordinary
	// frameless accessors keep their fast frameless path and pay nothing.
	if (vars.frameLess) {
		for (Variable **pVar = vars.order; pVar < vars.last; pVar++) {
			if ((*pVar)->reg == SPILLED_REG) {
				vars.frameLess = 0;
				break;
			}
		}
	}

	// NB: reads vars.frameLess, so it must come after the correction above.
	for (int16_t i = code->header.argsSize; i >= 0 ; i--) {
		alloc->vars[i + 1].frameOffset += (vars.frameLess ? 0 : 1) + 1; // +2 for return IC and saved BP
	}

#if PRINT_ALLOCATION
	printAllocation(&vars);
#endif

	alloc->regs = regs;
	alloc->varsSize = vars.varsSize;
	alloc->frameSize = -vars.frameSize - 1;
	alloc->frameLess = vars.frameLess;
}


static void printAllocation(Vars *vars)
{
	printf("Register allocation:\n     ");
	for (size_t j = 0; j < vars->varsSize; j++) {
		printf("  %2zX", j);
	}
	printf("\n");

	for (size_t i = 0; i < vars->maxOffset; i++) {
		size_t registers = 0;
		size_t spilled = 0;
		printf("%5zu", i);
		for (size_t j = 0; j < vars->varsSize; j++) {
			Variable *var = &vars->vars[j];
			if (var->start <= i && i <= var->end) {
				printf(" %3i", var->reg);
				if (var->reg == -1) {
					spilled++;
				} else {
					registers++;
				}
			} else {
				printf("    ");
			}
		}
		printf(" %2zu %2zu\n", registers, spilled);
	}
}


// A backward jump at bytecode `jump` closes a loop whose body starts at bytecode
// `target`. The textual [start, end] ranges scanCode computes are control-flow-blind:
// a variable whose last textual use precedes the back-edge is still live INTO the
// next iteration when its range reaches the loop body, but every allocating send
// between that last use and the back-edge gets a GC stackmap that omits it. A
// scavenge at such a send then leaves the variable's frame slot pointing into the
// abandoned semispace, and the next iteration reads a dangling object (e.g. a
// young Boolean flag tested at the loop top). Widen gcEnd — the stackmap filter's
// upper bound — of every range that overlaps [target, jump] to cover the whole
// loop. Only gcEnd moves: `end` still drives register allocation, so assignment
// and spill pressure are untouched, and the extra coverage is safe (the scavenger
// tolerates stale slots) at the cost of a few stackmap bits.
static void extendLoopVarRanges(Vars *vars, size_t target, size_t jump)
{
	for (Variable **pVar = vars->order; pVar < vars->last; pVar++) {
		Variable *var = *pVar;
		if (var->start <= jump && var->end >= target && var->gcEnd < jump) {
			var->gcEnd = jump;
		}
	}
}


static void scanCode(Vars *vars, CompiledCode *code)
{
	BytecodesIterator iterator;
	Bytecode bytecode;
	_Bool returns = 0;
	// Bytecode number at each instruction's start byte-offset: jump targets are
	// byte offsets, liveness ranges are bytecode numbers, and a backward target
	// was always visited before the jump that references it.
	ptrdiff_t *bnAtByte = calloc(code->bytecodesSize + 1, sizeof(ptrdiff_t));

	bytecodeInitIterator(&iterator, code->bytecodes, code->bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		ptrdiff_t startByte = bytecodeOffset(&iterator);
		bytecode = bytecodeNext(&iterator);
		bnAtByte[startByte] = bytecodeNumber(&iterator);
		switch (bytecode) {
		case BYTECODE_COPY:;
			Operand src = bytecodeNextOperand(&iterator);
			Operand dst = bytecodeNextOperand(&iterator);
			examineCopyOperands(vars, src, dst, bytecodeNumber(&iterator));
			break;

		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE:;
			vars->frameLess = 0;
			bytecodeNextByte(&iterator); // skip selector
			uint8_t argsSize = bytecodeNextByte(&iterator);

			Operand receiver = bytecodeNextOperand(&iterator);
			examineOperand(vars, receiver, bytecodeNumber(&iterator));

			for (uint8_t i = 0; i < argsSize; i++) {
				examineOperand(vars, bytecodeNextOperand(&iterator), bytecodeNumber(&iterator));
			}
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				examineOperand(vars, bytecodeNextOperand(&iterator), bytecodeNumber(&iterator) + 1);
			}
			break;

		case BYTECODE_RETURN:
			returns = 1;
			examineOperand(vars, bytecodeNextOperand(&iterator), bytecodeNumber(&iterator));
			break;

		case BYTECODE_OUTER_RETURN:
			vars->frameLess = 0;
			returns = 1;
			examineOperand(vars, bytecodeNextOperand(&iterator), bytecodeNumber(&iterator));
			break;

		case BYTECODE_JUMP: {
			// Relative displacement; the target byte offset is relative to the
			// post-read position (same recovery as generateBody).
			int32_t disp = bytecodeNextInt32(&iterator);
			if (disp < 0) {
				extendLoopVarRanges(vars, bnAtByte[bytecodeOffset(&iterator) + disp], bytecodeNumber(&iterator));
			}
			break;
		}

		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			bytecodeNextByte(&iterator); // skip literal
			examineOperand(vars, bytecodeNextOperand(&iterator), bytecodeNumber(&iterator));
			int32_t disp = bytecodeNextInt32(&iterator);
			if (disp < 0) {
				extendLoopVarRanges(vars, bnAtByte[bytecodeOffset(&iterator) + disp], bytecodeNumber(&iterator));
			}
			break;
		}

		default:
			FAIL();
		}
	}

	free(bnAtByte);

	if (!returns) {
		ptrdiff_t offset = bytecodeNumber(&iterator);
		defineTmpVar(vars, SELF_INDEX, offset == -1 ? 0 : offset);
	}
}


static void examineCopyOperands(Vars *vars, Operand src, Operand dst, size_t offset)
{
	examineOperand(vars, src, offset);
	examineOperand(vars, dst, offset);
}


// examineOperandClass() used to define a shadow VAR_CLASS variable for every send
// receiver, to reserve it a home for the receiver's cached class. That cache is gone:
// the backends recompute the class from the receiver at each send, because caching it
// is unsafe once inlined control flow exists (a send inside a conditional arm would
// populate the slot only on the taken path, and a later send would read the
// unpopulated slot as a garbage class -- see generateSend in CodeGeneratorX64.c).
//
// Defining the variable outlived the cache, and it was not free: each one took a
// register out of the same 9-entry pool, for a live range spanning first-to-last send
// on that receiver. Every send receiver burned a register for a cache nobody read,
// which is pure spill pressure -- and spill paths are where this JIT's bugs live.


static void examineOperand(Vars *vars, Operand operand, size_t offset)
{
	switch (operand.type) {
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
		defineTmpVar(vars, operand.index, offset);
		break;
	case OPERAND_SUPER:
		defineTmpVar(vars, SUPER_INDEX, offset);
		break;
	case OPERAND_CONTEXT_VAR:
		vars->frameLess = 0;
		defineSpecialVar(vars, VAR_CONTEXT, operand.level, offset);
		break;
	case OPERAND_ASSOC:
		defineSpecialVar(vars, VAR_ASSOC, operand.index, offset);
		break;
	case OPERAND_BLOCK:
		vars->frameLess = 0;
		defineTmpVar(vars, SELF_INDEX, offset);
		defineSpecialVar(vars, VAR_CONTEXT, 0, offset);
		break;
	case OPERAND_INST_VAR:
		defineTmpVar(vars, SELF_INDEX, offset);
		break;
	case OPERAND_INST_VAR_OF:
		// The tier-1 inliner's ivar form hangs off a spilled TEMP instance:
		// this use must extend that temp's live range, or its register is
		// recycled early and the access reads a stale home.
		defineTmpVar(vars, operand.instance.index, offset);
		break;
	default:
		;
	}
}


static void defineTmpVar(Vars *vars, uint8_t index, size_t offset)
{
	if ((vars->vars[index].flags & VAR_DEFINED) == 0) {
		defineVar(vars, VAR_TMP, index, offset);
	} else {
		vars->vars[index].end = offset;
		if (offset > vars->vars[index].gcEnd) {
			vars->vars[index].gcEnd = offset;
		}
	}
	if (offset > vars->maxOffset) {
		vars->maxOffset = offset;
	}
}


static Variable *defineSpecialVar(Vars *vars, uint8_t type, uint8_t index, size_t offset)
{
	if (vars->specialVars[type][index] == NULL) {
		if (type == VAR_CONTEXT) {
			vars->vars[CONTEXT_INDEX].end = offset;
			if (offset > vars->vars[CONTEXT_INDEX].gcEnd) {
				vars->vars[CONTEXT_INDEX].gcEnd = offset;
			}
		}
		vars->specialVars[type][index] = defineVar(vars, type, vars->varsSize++, offset);
	} else {
		vars->specialVars[type][index]->end = offset;
		if (offset > vars->specialVars[type][index]->gcEnd) {
			vars->specialVars[type][index]->gcEnd = offset;
		}
	}
	if (offset > vars->maxOffset) {
		vars->maxOffset = offset;
	}
	return vars->specialVars[type][index];
}


static Variable *defineVar(Vars *vars, uint8_t type, uint8_t index, size_t offset)
{
	Variable *var = &vars->vars[index];
	var->flags = VAR_DEFINED;
	var->index = index;
	var->reg = SPILLED_REG;
	var->start = var->end = var->gcEnd = offset;
	// VAR_ASSOC gets no frame slot on purpose: a spilled association is rebuilt from
	// the literal frame, never reloaded from the stack.
	if (type == VAR_TMP || type == VAR_CONTEXT) {
		var->frameOffset = vars->frameSize--;
	}
	*vars->last++ = var;
	ASSERT(vars->last < vars->order + 255);
	return var;
}


static void scanRegisters(Vars *vars, AvailableRegs *regs)
{
	SortedVars sortedVars;
	RegsPool regsPool;
	Variable *var;

	initSortedVars(&sortedVars);
	initRegsPool(&regsPool, regs);

	// The context variable is PINNED to the backend's dedicated context
	// register (x64: R12 = 12, ppc64: r30) — this used to be a hardcoded 12,
	// which is correct only on x64; on ppc64 register 12 is the send-target
	// scratch, so every context died at a method's first send.
	var = vars->specialVars[VAR_CONTEXT][0];
	var->reg = CTX;

	for (Variable **pVar = vars->order; pVar < vars->last; pVar++) {
		var = *pVar;
		if (var->reg != SPILLED_REG) {
			continue;
		}
		removeDeadVars(&sortedVars, &regsPool, var->start);

		if (getFirstVar(&sortedVars) == NULL || var->end >= getFirstVar(&sortedVars)->end) {
			addFirstVar(&sortedVars, var);
		} else {
			addLastVar(&sortedVars, var);
		}

		var->reg = nextReg(&regsPool);
		if (var->reg == SPILLED_REG) {
			Variable *spilledVar = removeFirstVar(&sortedVars);
			var->reg = spilledVar->reg;
			spilledVar->reg = SPILLED_REG;
		}
	}
}


static void initSortedVars(SortedVars *sortedVars)
{
	memset(sortedVars, 0, sizeof(*sortedVars));
	sortedVars->first = sortedVars->last = sortedVars->vars + SORTED_VARS_SIZE / 2;
}


static Variable *getFirstVar(SortedVars *sortedVars)
{
	return *sortedVars->first;
}


static void addFirstVar(SortedVars *sortedVars, Variable *var)
{
	ASSERT(sortedVars->first >= sortedVars->vars);
	*--sortedVars->first = var;
}


static void addLastVar(SortedVars *sortedVars, Variable *var)
{
	ASSERT(sortedVars->last < (sortedVars->vars + SORTED_VARS_SIZE));
	*sortedVars->last++ = var;
}


static void removeDeadVars(SortedVars *sortedVars, RegsPool *regsPool, size_t offset)
{
	Variable *var;

	for (Variable **pVar = sortedVars->first; pVar < sortedVars->last; ) {
		var = *pVar;
		if (var->end < offset) {
			reuseVarReg(regsPool, var);
			removeVar(sortedVars, pVar);
			// Do NOT advance: removeVar slid the tail down, so the next entry is
			// already at pVar. Advancing here skipped every run of consecutive dead
			// variables, leaving them in the active list with their registers never
			// returned to the pool, which spills more than necessary.
		} else {
			pVar++;
		}
	}
}


static void reuseVarReg(RegsPool *regsPool, Variable *var)
{
	// A spilled variable never took a register out of the pool, so it has none to
	// give back. Pushing its SPILLED_REG (-1) would both hand -1 out again as if it
	// were an allocatable register and drift `tmp` down without a matching nextReg(),
	// walking it off the front of the array once enough spilled variables die.
	if (var->reg == SPILLED_REG) {
		return;
	}
	*--regsPool->tmp = var->reg;
}


static void removeVar(SortedVars *sortedVars, Variable **var)
{
	// Move the tail (var, last) down onto `var`. The count is measured from var + 1,
	// not from var: the old form copied one entry too many and read one past `last`.
	memmove(var, var + 1, (int8_t *) sortedVars->last - (int8_t *) (var + 1));
	sortedVars->last--;
}


static Variable *removeFirstVar(SortedVars *sortedVars)
{
	Variable **pVar = sortedVars->first;
	Variable *var = *pVar;
	removeVar(sortedVars, pVar);
	return var;
}


// ST_JIT_REGS=<n> shrinks the allocatable pool to n registers. TEST ONLY: spilling
// is where this JIT's bugs hide (a spilled variable owns no register, and handing its
// SPILLED_REG (-1) to an emitter encodes a wild one), but at full width only unusually
// fat methods ever spill, so the whole class stays latent. Shrinking the pool forces
// ordinary code down the spill paths, which turns "latent" into a sweep:
//
//     for n in 3 4 5 6 7 8 9; do ST_JIT_REGS=$n ./run_tests.sh --all; done
//
// Read once and cached; unset (the default) means the full pool, so nothing changes
// for a normal build. Backend-independent: the pool comes from AvailableRegs.
static uint8_t availableRegsSize(AvailableRegs *regs)
{
	static int limit = -1;
	if (limit < 0) {
		const char *env = getenv("ST_JIT_REGS");
		limit = env != NULL ? atoi(env) : 0;
		if (limit < 1) {
			limit = 0; // 0 = no limit; a pool of 0 could not compile anything
		}
	}
	if (limit > 0 && limit < regs->regsSize) {
		return (uint8_t) limit;
	}
	return regs->regsSize;
}


static void initRegsPool(RegsPool *regsPool, AvailableRegs *regs)
{
	// regsPool->available = regs;
	uint8_t size = availableRegsSize(regs);
	memcpy(regsPool->regs, regs->regs, size);
	regsPool->tmp = regsPool->regs;
	regsPool->end = regsPool->tmp + size;
}


static uint8_t nextReg(RegsPool *regsPool)
{
	if (regsPool->tmp < regsPool->end) {
		return *regsPool->tmp++;
	}
	return SPILLED_REG;
}


void invalidateRegs(RegsAlloc *alloc)
{
	for (uint8_t i = 0; i < alloc->varsSize; i++) {
		alloc->vars[i].flags = alloc->vars[i].flags & ~VAR_IN_REG;
	}
}
