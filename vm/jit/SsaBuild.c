#include "jit/SsaBuild.h"
#include "core/Assert.h"
#include "runtime/Collection.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Backward liveness over the bytecode
// ---------------------------------------------------------------------------
//
// A deoptimization state carries the value of every register that is LIVE at
// its bytecode index, and nothing else. Without this the state would name every
// register of the frame, which has two consequences and both are fatal to the
// optimizer: the states get large, and every value they mention is kept alive,
// so dead-code elimination stops eliminating anything.
//
// Fixed-width instructions make this a plain array dataflow, with the
// instruction index as the coordinate.

typedef struct {
	uint16_t *uses;   // registers read, terminated by BYTECODE_NO_TARGET
	uint16_t define;  // register written, or BYTECODE_NO_TARGET
	uint16_t useCount;
} UseDef;


static void instructionUseDef(const Instruction *instruction, uint16_t *uses,
	uint16_t *useCount, uint16_t *define)
{
	*useCount = 0;
	*define = BYTECODE_NO_TARGET;
	switch ((Opcode) instruction->op) {
	case OP_MOVE:
		uses[(*useCount)++] = instruction->b;
		*define = instruction->a;
		break;
	case OP_LOADK: case OP_LOADI: case OP_LOADNIL:
	case OP_LOADTRUE: case OP_LOADFALSE: case OP_GETGLOBAL:
		*define = instruction->a;
		break;
	case OP_GETIVAR:
		uses[(*useCount)++] = instruction->b;
		*define = instruction->a;
		break;
	case OP_SETIVAR:
		uses[(*useCount)++] = instruction->a;
		uses[(*useCount)++] = instruction->c;
		break;
	case OP_SETGLOBAL:
		uses[(*useCount)++] = instruction->b;
		break;
	case OP_SEND: case OP_SENDSUPER:
		// The receiver and its consecutive arguments.
		for (uint16_t i = 0; i <= instruction->n; i++) {
			uses[(*useCount)++] = (uint16_t) (instruction->c + i);
		}
		*define = instruction->a;
		break;
	case OP_JUMPFALSE: case OP_JUMPTRUE: case OP_GUARDCLASS:
	case OP_RET: case OP_RETOUTER:
		uses[(*useCount)++] = instruction->a;
		break;
	case OP_NEW:
		*define = instruction->a;
		break;
	case OP_NEWIDX:
		uses[(*useCount)++] = instruction->c;
		*define = instruction->a;
		break;
	case OP_JUMP: case OP_SAFEPOINT:
		break;
	default:
		break;
	}
}


// live[i] is the set of registers live on ENTRY to instruction i, as a bitset.
static uint64_t *computeLiveness(CodeUnit *unit)
{
	uint16_t count = unit->instructionCount;
	uint16_t words = (uint16_t) ((unit->registerCount + 63) / 64);
	uint64_t *live = calloc((size_t) count * words, sizeof(uint64_t));
	ASSERT(live != NULL);

	uint16_t uses[64];
	uint16_t useCount, define;
	_Bool changed = 1;
	while (changed) {
		changed = 0;
		// Backward, because liveness flows against control flow and a backward
		// sweep converges in one pass for straight-line code.
		for (int32_t i = count - 1; i >= 0; i--) {
			Instruction *instruction = &unit->code[i];
			uint64_t *out = calloc(words, sizeof(uint64_t));

			if (opcodeFallsThrough((Opcode) instruction->op) && i + 1 < count) {
				for (uint16_t w = 0; w < words; w++) {
					out[w] |= live[(size_t) (i + 1) * words + w];
				}
			}
			uint16_t target = opcodeBranchTarget(instruction);
			if (target != BYTECODE_NO_TARGET && target < count) {
				for (uint16_t w = 0; w < words; w++) {
					out[w] |= live[(size_t) target * words + w];
				}
			}

			instructionUseDef(instruction, uses, &useCount, &define);
			if (define != BYTECODE_NO_TARGET) {
				out[define / 64] &= ~((uint64_t) 1 << (define % 64));
			}
			for (uint16_t u = 0; u < useCount; u++) {
				out[uses[u] / 64] |= (uint64_t) 1 << (uses[u] % 64);
			}

			for (uint16_t w = 0; w < words; w++) {
				if (live[(size_t) i * words + w] != out[w]) {
					live[(size_t) i * words + w] = out[w];
					changed = 1;
				}
			}
			free(out);
		}
	}
	return live;
}


// ---------------------------------------------------------------------------
// SSA construction, Braun et al.
// ---------------------------------------------------------------------------
//
// Chosen over dominance frontiers because it is much shorter and because it
// builds SSA DURING the lowering walk, in one pass, instead of needing a
// separate CFG analysis first.

typedef struct {
	IrFunction *function;
	CodeUnit *unit;
	IrBlock **blockAt;   // bytecode index -> block starting there, or NULL
	uint64_t *live;
	uint16_t liveWords;
	IrBlock *current;
	// CONDITIONAL class knowledge: value id -> class established BY A GUARD.
	// Deliberately NOT on the value (see the header of Ir.h): a guard whose
	// established fact is written onto the value makes the redundant-guard pass
	// delete that very guard.
	uint32_t *known;
	uint32_t knownCapacity;
} Builder;


static void writeVariable(Builder *builder, uint16_t reg, IrBlock *block,
	IrValue *value)
{
	// `builder` is unused: a write is a store into the block's own def table and
	// needs nothing global, while a READ may have to walk predecessors and build
	// a phi. The parameter stays for the pairing with readVariable, which is the
	// half of Braun's algorithm that this one is only meaningful next to.
	(void) builder;
	block->defs[reg] = value;
}


static IrValue *readVariable(Builder *builder, uint16_t reg, IrBlock *block);


static void addPhiOperands(Builder *builder, uint16_t reg, IrValue *phi)
{
	IrBlock *block = phi->block;
	for (uint16_t i = 0; i < block->predCount; i++) {
		irAddArg(builder->function, phi, readVariable(builder, reg, block->preds[i]));
	}
}


static IrValue *readVariableRecursive(Builder *builder, uint16_t reg, IrBlock *block)
{
	IrValue *value;
	if (!block->sealed) {
		// Not every predecessor is known yet, so the phi's operands cannot be
		// filled in. Record it and come back at seal time.
		value = irNewValue(builder->function, IR_PHI);
		value->extra = reg;
		irAppend(block, value);
		block->incompletePhis[reg] = value;
	} else if (block->predCount == 1) {
		value = readVariable(builder, reg, block->preds[0]);
	} else if (block->predCount == 0) {
		// No predecessor and no definition: the register is nil here.
		value = irNewValue(builder->function, IR_CONST);
		value->konst = 0;
		irAppend(builder->function->entry, value);
	} else {
		value = irNewValue(builder->function, IR_PHI);
		value->extra = reg;
		irAppend(block, value);
		writeVariable(builder, reg, block, value);
		addPhiOperands(builder, reg, value);
	}
	writeVariable(builder, reg, block, value);
	return value;
}


static IrValue *readVariable(Builder *builder, uint16_t reg, IrBlock *block)
{
	IrValue *value = block->defs[reg];
	if (value != NULL) {
		return value;
	}
	return readVariableRecursive(builder, reg, block);
}


static void sealBlock(Builder *builder, IrBlock *block)
{
	if (block->sealed) {
		return;
	}
	block->sealed = 1;
	for (uint16_t reg = 0; reg < builder->function->registerCount; reg++) {
		IrValue *phi = block->incompletePhis[reg];
		if (phi != NULL) {
			addPhiOperands(builder, reg, phi);
			block->incompletePhis[reg] = NULL;
		}
	}
}


// ---------------------------------------------------------------------------
// CFG
// ---------------------------------------------------------------------------

static void addPredecessor(IrFunction *function, IrBlock *block, IrBlock *pred)
{
	if (block->predCount == block->predCapacity) {
		uint16_t capacity = block->predCapacity == 0 ? 2
			: (uint16_t) (block->predCapacity * 2);
		IrBlock **preds = irAlloc(function, capacity * sizeof(IrBlock *));
		if (block->preds != NULL) {
			memcpy(preds, block->preds, block->predCount * sizeof(IrBlock *));
		}
		block->preds = preds;
		block->predCapacity = capacity;
	}
	block->preds[block->predCount++] = pred;
}


static void buildCfg(Builder *builder)
{
	CodeUnit *unit = builder->unit;
	uint16_t count = unit->instructionCount;
	_Bool *leader = calloc(count, sizeof(_Bool));
	leader[0] = 1;
	for (uint16_t i = 0; i < count; i++) {
		uint16_t target = opcodeBranchTarget(&unit->code[i]);
		if (target != BYTECODE_NO_TARGET && target < count) {
			leader[target] = 1;
		}
		if (opcodeIsTerminator((Opcode) unit->code[i].op) && i + 1 < count) {
			leader[i + 1] = 1;
		}
	}

	for (uint16_t i = 0; i < count; i++) {
		if (leader[i]) {
			builder->blockAt[i] = irNewBlock(builder->function, i);
		}
	}
	builder->function->entry = builder->blockAt[0];

	// Successors: walk each block to its terminator or to the next leader.
	for (uint16_t i = 0; i < count; i++) {
		IrBlock *block = builder->blockAt[i];
		if (block == NULL) {
			continue;
		}
		uint16_t end = (uint16_t) (i + 1);
		while (end < count && builder->blockAt[end] == NULL) {
			end++;
		}
		Instruction *last = &unit->code[end - 1];
		uint16_t target = opcodeBranchTarget(last);
		if (target != BYTECODE_NO_TARGET && target < count) {
			block->succs[block->succCount++] = builder->blockAt[target];
		}
		if (opcodeFallsThrough((Opcode) last->op) && end < count) {
			block->succs[block->succCount++] = builder->blockAt[end];
		}
	}
	for (IrBlock *block = builder->function->blocks; block != NULL;
			block = block->next) {
		for (uint8_t s = 0; s < block->succCount; s++) {
			addPredecessor(builder->function, block->succs[s], block);
		}
	}
	free(leader);
}


// ---------------------------------------------------------------------------
// Deoptimization state
// ---------------------------------------------------------------------------

// Capture the live registers at `bci`. ONE frame for now: inlining is what adds
// outer frames, and the shape is already the one it needs, with `innermost`
// distinguishing "re-execute this instruction" from "resume after the send".
static DeoptState *captureDeopt(Builder *builder, uint16_t bci)
{
	IrFunction *function = builder->function;
	DeoptState *state = irAlloc(function, sizeof(DeoptState));
	state->frames = irAlloc(function, sizeof(DeoptFrame));
	state->frameCount = 1;

	DeoptFrame *frame = &state->frames[0];
	frame->unit = builder->unit;
	frame->bci = bci;
	frame->innermost = 1;
	frame->destRegister = BYTECODE_NO_TARGET;

	uint64_t *live = &builder->live[(size_t) bci * builder->liveWords];
	uint16_t count = 0;
	for (uint16_t reg = 0; reg < function->registerCount; reg++) {
		if (live[reg / 64] & ((uint64_t) 1 << (reg % 64))) {
			count++;
		}
	}
	frame->slotCount = count;
	frame->slotRegister = irAlloc(function, count * sizeof(uint16_t));
	frame->slotValue = irAlloc(function, count * sizeof(IrValue *));
	uint16_t index = 0;
	for (uint16_t reg = 0; reg < function->registerCount; reg++) {
		if (live[reg / 64] & ((uint64_t) 1 << (reg % 64))) {
			frame->slotRegister[index] = reg;
			frame->slotValue[index] = readVariable(builder, reg, builder->current);
			index++;
		}
	}
	return state;
}


// ---------------------------------------------------------------------------
// Lowering
// ---------------------------------------------------------------------------

static IrValue *emit(Builder *builder, IrOp op)
{
	IrValue *value = irNewValue(builder->function, op);
	irAppend(builder->current, value);
	return value;
}


static void lower(Builder *builder, uint16_t bci)
{
	Instruction *instruction = &builder->unit->code[bci];
	IrBlock *block = builder->current;
	IrFunction *function = builder->function;

	switch ((Opcode) instruction->op) {
	case OP_MOVE:
		// No node at all: an SSA name is just rebound. This is why the register
		// bytecode's MOVEs cost nothing after construction.
		writeVariable(builder, instruction->a, block,
			readVariable(builder, instruction->b, block));
		break;

	case OP_LOADI: {
		IrValue *value = emit(builder, IR_CONST);
		value->konst = tagInt((int16_t) instruction->b);
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_LOADNIL: case OP_LOADTRUE: case OP_LOADFALSE: case OP_LOADK: {
		IrValue *value = emit(builder, IR_CONST);
		value->extra = instruction->op;
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_GETIVAR: {
		IrValue *value = emit(builder, IR_FIELD_T);
		irAddArg(function, value, readVariable(builder, instruction->b, block));
		value->extra = instruction->c;
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_SETIVAR: {
		IrValue *value = emit(builder, IR_SETFIELD_T);
		irAddArg(function, value, readVariable(builder, instruction->a, block));
		irAddArg(function, value, readVariable(builder, instruction->c, block));
		value->extra = instruction->b;
		break;
	}

	case OP_SEND: case OP_SENDSUPER: {
		IrValue *value = emit(builder, IR_SEND);
		for (uint16_t i = 0; i <= instruction->n; i++) {
			irAddArg(function, value,
				readVariable(builder, (uint16_t) (instruction->c + i), block));
		}
		value->extra = instruction->b;
		// A send can leave optimized code, so it carries the state to resume
		// with. Attached at construction, not retrofitted later: a state added
		// after the optimizer has run describes a frame that no longer exists.
		value->deopt = captureDeopt(builder, bci);
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_GUARDCLASS: {
		IrValue *receiver = readVariable(builder, instruction->a, block);
		IrValue *guard = emit(builder, IR_GUARD_CLASS);
		irAddArg(function, guard, receiver);
		guard->extra = instruction->b;
		guard->deopt = captureDeopt(builder, bci);
		// The established fact goes in the builder's `known` map, NEVER on the
		// value. See Ir.h: on the value it would make the redundant-guard pass
		// delete the guard that established it.
		if (receiver->id < builder->knownCapacity) {
			builder->known[receiver->id] = (uint32_t) instruction->b;
		}
		break;
	}

	case OP_SAFEPOINT: {
		IrValue *value = emit(builder, IR_SAFEPOINT);
		value->deopt = captureDeopt(builder, bci);
		break;
	}

	case OP_RET: {
		IrValue *value = irNewValue(function, IR_RET);
		irAddArg(function, value, readVariable(builder, instruction->a, block));
		value->block = block;
		block->terminator = value;
		break;
	}

	case OP_JUMP: {
		IrValue *value = irNewValue(function, IR_JUMP);
		value->block = block;
		block->terminator = value;
		break;
	}

	case OP_JUMPFALSE: case OP_JUMPTRUE: {
		IrValue *value = irNewValue(function, IR_BRANCH);
		irAddArg(function, value, readVariable(builder, instruction->a, block));
		value->extra = instruction->op == OP_JUMPTRUE;
		value->block = block;
		block->terminator = value;
		break;
	}

	default:
		break;
	}
}


IrFunction *ssaBuild(CodeUnit *unit)
{
	Builder builder;
	memset(&builder, 0, sizeof(builder));
	builder.function = irCreate(unit);
	builder.unit = unit;
	builder.blockAt = calloc(unit->instructionCount, sizeof(IrBlock *));
	builder.live = computeLiveness(unit);
	builder.liveWords = (uint16_t) ((unit->registerCount + 63) / 64);
	builder.knownCapacity = 4096;
	builder.known = calloc(builder.knownCapacity, sizeof(uint32_t));

	buildCfg(&builder);

	// Parameters: the receiver and the declared arguments are defined on entry.
	IrBlock *entry = builder.function->entry;
	for (uint16_t reg = 0; reg <= unit->argumentCount; reg++) {
		IrValue *param = irNewValue(builder.function, IR_PARAM);
		param->extra = reg;
		irAppend(entry, param);
		writeVariable(&builder, reg, entry, param);
	}

	// Fill blocks in bytecode order. A block is sealed once every predecessor
	// has been filled, which for a loop header means after the back edge is
	// seen: that is precisely the case Braun's incomplete phis exist for.
	uint16_t count = unit->instructionCount;
	for (uint16_t i = 0; i < count; i++) {
		if (builder.blockAt[i] != NULL) {
			builder.current = builder.blockAt[i];
			_Bool ready = 1;
			for (uint16_t p = 0; p < builder.current->predCount; p++) {
				ready = ready && builder.current->preds[p]->filled;
			}
			if (ready) {
				sealBlock(&builder, builder.current);
			}
		}
		lower(&builder, i);
		if (i + 1 == count || builder.blockAt[i + 1] != NULL) {
			builder.current->filled = 1;
		}
	}
	// Anything still unsealed is a loop header whose back edge has now been
	// seen.
	for (IrBlock *block = builder.function->blocks; block != NULL;
			block = block->next) {
		builder.current = block;
		sealBlock(&builder, block);
	}

	IrFunction *function = builder.function;
	free(builder.blockAt);
	free(builder.live);
	free(builder.known);
	return function;
}
