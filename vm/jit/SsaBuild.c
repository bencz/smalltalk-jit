#include "jit/SsaBuild.h"
#include "core/Assert.h"
#include <stdio.h>
#include "runtime/Collection.h"
#include "runtime/Closure.h"
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
	case OP_GETUP:
		// Register 0 of a block's frame IS the running closure (jit/Jit.c), so
		// reading a capture reads a field of it.
		uses[(*useCount)++] = 0;
		*define = instruction->a;
		break;
	case OP_NEWCELL:
		uses[(*useCount)++] = instruction->b;
		*define = instruction->a;
		break;
	case OP_GETCELL:
		uses[(*useCount)++] = instruction->b;
		*define = instruction->a;
		break;
	case OP_SETCELL:
		uses[(*useCount)++] = instruction->a; // the cell
		uses[(*useCount)++] = instruction->b; // the value
		break;
	case OP_CLOSURE:
		// The captures are consecutive registers starting at c, gathered there
		// by the front end for the same reason a send's arguments are.
		for (uint16_t i = 0; i < instruction->n; i++) {
			uses[(*useCount)++] = (uint16_t) (instruction->c + i);
		}
		*define = instruction->a;
		break;
	case OP_JUMP: case OP_SAFEPOINT:
		break;
	default:
		// Same contract as the emit switch below: ssaBuild refused everything
		// this does not name before calling it. An opcode arriving here would
		// contribute no uses and no definition, so the liveness -- and every
		// deopt state derived from it -- would silently omit its registers.
		fprintf(stderr, "ssaBuild: opcode %s is modelled but has no def/use\n",
			opcodeName((Opcode) instruction->op));
		FAIL();
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
		// WHICH KIND of constant, written down rather than left as the zero the
		// allocator gave it. The three singletons below set this field, so a
		// reader that wants to tell an integer literal from nil has to have it
		// here too: `konst` is zero for nil and zero is a valid tagged
		// SmallInteger, so the value alone cannot answer.
		value->extra = OP_LOADI;
		value->konst = tagInt((int16_t) instruction->b);
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_LOADNIL: case OP_LOADTRUE: case OP_LOADFALSE: {
		// The three singletons. Their ADDRESSES are runtime facts, so the value
		// is not known here, but WHICH singleton is -- and that is all GVN needs
		// to be right about collapsing two of them together.
		IrValue *value = emit(builder, IR_CONST);
		value->extra = instruction->op;
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_LOADK: {
		// The literal INDEX, which is the thing that has to survive. It used to
		// share IR_CONST with the three above and set `extra` to the opcode, so
		// the index went nowhere and every literal load in a method looked
		// identical to GVN. See IR_LITERAL in jit/Ir.h.
		IrValue *value = emit(builder, IR_LITERAL);
		value->extra = instruction->b;
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

	case OP_GETUP: {
		// A CAPTURED VALUE, and modelling it as an ordinary tagged field read is
		// only sound because a capture is IMMUTABLE. ADR 0008 is what makes it
		// so: a variable that is captured and then ASSIGNED does not live in the
		// closure at all, it gets a heap CELL and the closure captures the cell's
		// address. So nothing ever writes captured[i] after the closure is built,
		// and treating the read as pure -- which is what lets GVN and LICM move
		// it -- cannot be invalidated by a store that does not exist.
		//
		// That reasoning does NOT extend to GETCELL, which is why the cell
		// opcodes are still refused: a cell exists precisely to be written, and
		// this optimizer's GVN has no memory effects yet.
		IrValue *value = emit(builder, IR_FIELD_T);
		irAddArg(function, value, readVariable(builder, 0, block));
		value->extra = CLOSURE_CAPTURE_FIELD(instruction->b);
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_NEWCELL: {
		// AN EXPLICIT ALLOCATION, which is the whole reason ADR 0008 chose cells
		// over contexts: escape analysis can only erase what it can see. It does
		// not erase this one yet -- that needs a materialization recipe -- but
		// the shape is here for when it does.
		IrValue *value = emit(builder, IR_NEWCELL);
		irAddArg(function, value, readVariable(builder, instruction->b, block));
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_GETCELL: {
		IrValue *value = emit(builder, IR_FIELD_T);
		irAddArg(function, value, readVariable(builder, instruction->b, block));
		value->extra = CELL_VALUE_FIELD;
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_SETCELL: {
		IrValue *value = emit(builder, IR_SETFIELD_T);
		irAddArg(function, value, readVariable(builder, instruction->a, block));
		irAddArg(function, value, readVariable(builder, instruction->b, block));
		value->extra = CELL_VALUE_FIELD;
		break;
	}

	case OP_CLOSURE: {
		IrValue *value = emit(builder, IR_CLOSURE);
		for (uint16_t i = 0; i < instruction->n; i++) {
			irAddArg(function, value,
				readVariable(builder, (uint16_t) (instruction->c + i), block));
		}
		value->extra = instruction->b;
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_GETGLOBAL: {
		// The Association's VALUE, and the association itself never becomes an IR
		// operand: it is a heap object that moves, so the backend reaches it the
		// way tier 1 does, through the address of the unit's literal-frame field
		// (jit/Jit.c). What the IR carries is the literal INDEX, which is stable.
		IrValue *value = emit(builder, IR_GLOBAL);
		value->extra = instruction->b;
		writeVariable(builder, instruction->a, block, value);
		break;
	}

	case OP_SETGLOBAL: {
		IrValue *value = emit(builder, IR_SETGLOBAL);
		irAddArg(function, value, readVariable(builder, instruction->b, block));
		value->extra = instruction->a;
		break;
	}

	case OP_SEND: case OP_SENDSUPER: {
		IrValue *value = emit(builder, IR_SEND);
		for (uint16_t i = 0; i <= instruction->n; i++) {
			irAddArg(function, value,
				readVariable(builder, (uint16_t) (instruction->c + i), block));
		}
		value->extra = instruction->b;
		// WHICH SITE, so the backend finds this send's cache cell, which is
		// indexed by bytecode index; and WHETHER IT IS SUPER, which used to be
		// dropped here because both opcodes shared this arm. See IR_FLAG_SUPER
		// in jit/Ir.h for why dropping it is a wrong answer and not a missed
		// optimization.
		value->bci = bci;
		if ((Opcode) instruction->op == OP_SENDSUPER) {
			value->flags |= IR_FLAG_SUPER;
		}
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
		guard->bci = bci;
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
		value->bci = bci;
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

	case OP_RETOUTER: {
		IrValue *value = irNewValue(function, IR_RETOUTER);
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
		// UNREACHABLE: ssaBuild refuses anything ssaModels does not name, before
		// a single node is built. Reaching here means the two disagree, which is
		// an opcode added to that list and not to this switch -- the silent drop
		// this whole arrangement exists to prevent, so it stops here loudly.
		fprintf(stderr, "ssaBuild: opcode %s is modelled but not emitted\n",
			opcodeName((Opcode) instruction->op));
		FAIL();
	}
}


// Every opcode this builder MODELS.
//
// A LIST, CHECKED UP FRONT, and not a `default:` arm in each of the two switches
// above. Those both end in `default: break`, which for an opcode nobody taught
// them means the instruction is SILENTLY SKIPPED: its effect vanishes from the
// IR and its registers vanish from the liveness. That is not a missed
// optimization, it is a method whose closure operations were dropped, answering
// wrongly, with nothing anywhere reporting a problem.
//
// The bytecode grew after this builder was written. `CLOSURE`, `NEWCELL`,
// `GETCELL`, `SETCELL`, `GETUP` and `SETUP` (ADR 0008) are exactly the opcodes
// that arrived later and are not modelled here, which is why this check exists
// before anything is built rather than as an assertion inside the walk: the
// answer for those is REFUSE, by name, the same way jitCompile refuses an
// opcode the template compiler does not implement.
// THE LIST IS THE EMIT SWITCH'S, and getting that wrong is the mistake this
// comment exists to stop. The first version of it was copied from the def/use
// scanner above, which knows about five opcodes the emit switch does NOT build
// anything for (GETGLOBAL, SETGLOBAL, NEW, NEWIDX, RETOUTER). Those would have
// passed the check and then been dropped in silence, which is the exact defect
// the check was added to remove -- so the emit switch now ABORTS on anything
// that gets past here, and the two cannot drift apart quietly again.
static _Bool ssaModels(Opcode op)
{
	switch (op) {
	case OP_MOVE: case OP_LOADK: case OP_LOADI: case OP_LOADNIL:
	case OP_LOADTRUE: case OP_LOADFALSE:
	case OP_GETIVAR: case OP_SETIVAR:
	case OP_GETUP:
	case OP_NEWCELL: case OP_GETCELL: case OP_SETCELL: case OP_CLOSURE:
	case OP_RETOUTER:
	case OP_GETGLOBAL: case OP_SETGLOBAL:
	case OP_SEND: case OP_SENDSUPER:
	case OP_JUMP: case OP_JUMPFALSE: case OP_JUMPTRUE: case OP_GUARDCLASS:
	case OP_RET:
	case OP_SAFEPOINT:
		return 1;
	default:
		return 0;
	}
}


IrFunction *ssaBuild(CodeUnit *unit, Opcode *unsupported)
{
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		Opcode op = (Opcode) unit->code[i].op;
		if (!ssaModels(op)) {
			if (unsupported != NULL) {
				*unsupported = op;
			}
			return NULL;
		}
	}

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
