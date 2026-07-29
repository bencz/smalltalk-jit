#include "jit/Lir.h"
#include "jit/Deopt.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Arena chunks, for the same reason jit/Ir.c has them: the LIR lives exactly as
// long as one compilation, so nothing is ever individually freed and an
// instruction the allocator unlinked can stay referenced with no ownership
// question to answer.
typedef struct LirArenaChunk {
	struct LirArenaChunk *next;
	size_t used, capacity;
	uint8_t bytes[];
} LirArenaChunk;

#define ARENA_CHUNK_BYTES (64 * 1024)


static const struct {
	const char *name;
	_Bool terminator;
	_Bool canLeave;
} gOps[LIR_OP_COUNT] = {
	[LIR_MOVE]        = { "move",        0, 0 },
	[LIR_IMM]         = { "imm",         0, 0 },
	[LIR_FIMM]        = { "fimm",        0, 0 },

	[LIR_LOAD]        = { "load",        0, 0 },
	[LIR_STORE]       = { "store",       0, 0 },
	[LIR_LOAD32]      = { "load32",      0, 0 },
	[LIR_STORE32]     = { "store32",     0, 0 },
	[LIR_LOAD_SLOT]   = { "load_slot",   0, 0 },
	[LIR_STORE_SLOT]  = { "store_slot",  0, 0 },
	[LIR_LOAD_ABS]    = { "load_abs",    0, 0 },
	[LIR_FRAME_ADDR]  = { "frame_addr",  0, 0 },

	[LIR_ADD]         = { "add",         0, 0 },
	[LIR_SUB]         = { "sub",         0, 0 },
	[LIR_MUL]         = { "mul",         0, 0 },
	[LIR_DIV]         = { "div",         0, 0 },
	[LIR_MOD]         = { "mod",         0, 0 },
	[LIR_AND]         = { "and",         0, 0 },
	[LIR_OR]          = { "or",          0, 0 },
	[LIR_XOR]         = { "xor",         0, 0 },
	[LIR_SHL]         = { "shl",         0, 0 },
	[LIR_SAR]         = { "sar",         0, 0 },
	[LIR_ADDI]        = { "addi",        0, 0 },
	[LIR_ANDI]        = { "andi",        0, 0 },
	[LIR_SHLI]        = { "shli",        0, 0 },
	[LIR_SARI]        = { "sari",        0, 0 },
	[LIR_NEG]         = { "neg",         0, 0 },

	[LIR_FADD]        = { "fadd",        0, 0 },
	[LIR_FSUB]        = { "fsub",        0, 0 },
	[LIR_FMUL]        = { "fmul",        0, 0 },
	[LIR_FDIV]        = { "fdiv",        0, 0 },
	[LIR_FNEG]        = { "fneg",        0, 0 },
	[LIR_FSQRT]       = { "fsqrt",       0, 0 },
	[LIR_I2F]         = { "i2f",         0, 0 },
	[LIR_F2I]         = { "f2i",         0, 0 },
	[LIR_BITCAST_I2F] = { "bitcast_i2f", 0, 0 },
	[LIR_BITCAST_F2I] = { "bitcast_f2i", 0, 0 },

	[LIR_JUMP]            = { "jump",            1, 0 },
	[LIR_CMP_BRANCH]      = { "cmp_branch",      1, 0 },
	[LIR_CMP_BRANCH_IMM]  = { "cmp_branch_imm",  1, 0 },
	[LIR_CMP_SET]         = { "cmp_set",         0, 0 },
	[LIR_RET]             = { "ret",             1, 0 },

	// The calls are NOT terminators even though a primitive that succeeds
	// returns and a runtime helper may not come back at all. A terminator here
	// means "the block ends", and the block does not: the very next instruction
	// is where a failed primitive continues and where a returning call lands.
	// Marking them otherwise would break the successor walk for the ordinary
	// case in order to describe an exit the CFG does not model anyway.
	[LIR_CALL_RUNTIME3]   = { "call_runtime3",   0, 1 },
	[LIR_CALL_TARGET]     = { "call_target",     0, 1 },
	[LIR_CALL_PRIMITIVE]  = { "call_primitive",  0, 1 },

	[LIR_GUARD_CLASS]     = { "guard_class",     0, 1 },
	[LIR_SAFEPOINT]       = { "safepoint",       0, 1 },
	[LIR_ENTRY]           = { "entry",           0, 0 },
};


const char *lirOpName(LirOp op)
{
	return op < LIR_OP_COUNT && gOps[op].name != NULL ? gOps[op].name : "?";
}


_Bool lirOpIsTerminator(LirOp op)
{
	return op < LIR_OP_COUNT && gOps[op].terminator;
}


_Bool lirOpCanLeave(LirOp op)
{
	return op < LIR_OP_COUNT && gOps[op].canLeave;
}


_Bool lirOpClobbers(LirOp op)
{
	return lirOpCanLeave(op) || op == LIR_DIV || op == LIR_MOD;
}


// One site, and everything hanging off it. Three allocations rather than one
// flexible-array blob, because the frame count and the slot counts are only
// known one at a time as the state is walked.
void lirFreeDeoptSite(struct DeoptSite *site)
{
	if (site == NULL) {
		return;
	}
	for (uint16_t f = 0; f < site->frameCount; f++) {
		free(site->frames[f].slots);
	}
	free(site->frames);
	free(site);
}


// ---- arena ------------------------------------------------------------------

void *lirAlloc(LirFunction *function, size_t bytes)
{
	bytes = (bytes + 7) & ~(size_t) 7;
	LirArenaChunk *chunk = function->chunks;
	if (chunk == NULL || chunk->used + bytes > chunk->capacity) {
		size_t capacity = bytes > ARENA_CHUNK_BYTES ? bytes : ARENA_CHUNK_BYTES;
		chunk = malloc(sizeof(LirArenaChunk) + capacity);
		ASSERT(chunk != NULL);
		chunk->next = function->chunks;
		chunk->used = 0;
		chunk->capacity = capacity;
		function->chunks = chunk;
	}
	void *result = chunk->bytes + chunk->used;
	chunk->used += bytes;
	memset(result, 0, bytes);
	return result;
}


LirFunction *lirCreate(struct CodeUnit *unit, const Abi *abi)
{
	LirFunction *function = calloc(1, sizeof(LirFunction));
	ASSERT(function != NULL);
	function->unit = unit;
	function->abi = abi;
	return function;
}


void lirDestroy(LirFunction *function)
{
	if (function == NULL) {
		return;
	}
	LirArenaChunk *chunk = function->chunks;
	while (chunk != NULL) {
		LirArenaChunk *next = chunk->next;
		free(chunk);
		chunk = next;
	}
	free(function->vregBank);
	free(function->vregKind);
	free(function->order);
	free(function->deoptVregOf);
	if (!function->deoptSitesTransferred) {
		for (uint32_t i = 0; i < function->deoptSiteCount; i++) {
			lirFreeDeoptSite(function->deoptSites[i]);
		}
	}
	free(function->deoptSites);
	free(function);
}


// ---- building ---------------------------------------------------------------

LirBlock *lirNewBlock(LirFunction *function, uint16_t label)
{
	LirBlock *block = lirAlloc(function, sizeof(LirBlock));
	block->id = function->blockCount++;
	block->label = label;
	block->from = block->to = -1;
	// Appended at the TAIL, so the list is in creation order and the lowering
	// can rely on that when it needs a block to be laid out after another.
	LirBlock **tail = &function->blocks;
	while (*tail != NULL) {
		tail = &(*tail)->next;
	}
	*tail = block;
	if (function->entry == NULL) {
		function->entry = block;
	}
	return block;
}


void lirAddEdge(LirFunction *function, LirBlock *from, LirBlock *to)
{
	ASSERT(from->succCount < 2);
	from->succs[from->succCount++] = to;
	if (to->predCount == to->predCapacity) {
		uint16_t capacity = to->predCapacity == 0 ? 4
			: (uint16_t) (to->predCapacity * 2);
		LirBlock **preds = lirAlloc(function, capacity * sizeof(LirBlock *));
		memcpy(preds, to->preds, to->predCount * sizeof(LirBlock *));
		to->preds = preds;
		to->predCapacity = capacity;
	}
	to->preds[to->predCount++] = from;
}


static LirInstruction *newInstruction(LirFunction *function, LirOp op)
{
	LirInstruction *instruction = lirAlloc(function, sizeof(LirInstruction));
	instruction->op = (uint16_t) op;
	instruction->dst = LIR_NO_VREG;
	instruction->args[0] = LIR_NO_VREG;
	instruction->args[1] = LIR_NO_VREG;
	instruction->dstReg = LIR_NO_REG;
	instruction->argReg[0] = LIR_NO_REG;
	instruction->argReg[1] = LIR_NO_REG;
	instruction->position = -1;
	return instruction;
}


LirInstruction *lirAppend(LirFunction *function, LirBlock *block, LirOp op)
{
	LirInstruction *instruction = newInstruction(function, op);
	if (block->last == NULL) {
		block->first = block->last = instruction;
	} else {
		block->last->next = instruction;
		block->last = instruction;
	}
	return instruction;
}


LirInstruction *lirInsertBefore(LirFunction *function, LirBlock *block,
	LirInstruction *before, LirOp op)
{
	if (before == NULL) {
		return lirAppend(function, block, op);
	}
	LirInstruction *instruction = newInstruction(function, op);
	if (block->first == before) {
		instruction->next = before;
		block->first = instruction;
		return instruction;
	}
	LirInstruction *previous = block->first;
	while (previous != NULL && previous->next != before) {
		previous = previous->next;
	}
	ASSERT(previous != NULL); // `before` was not in this block
	instruction->next = before;
	previous->next = instruction;
	return instruction;
}


uint32_t lirNewVreg(LirFunction *function, LirBank bank, SlotKind kind)
{
	// A float-bank value is always raw bytes, and a collector that scanned one
	// would be reading a double as a pointer, which is precisely requirement R1.
	// Checked rather than commented, because the caller supplies both and the
	// combination that is wrong is silent.
	ASSERT(bank != LIR_BANK_FLOAT || kind == SLOT_F64);
	ASSERT(bank != LIR_BANK_INT || kind != SLOT_F64);
	if ((function->vregCount & (function->vregCount - 1)) == 0) {
		// Doubling, at every power of two. A realloc per vreg would be quadratic
		// copying on a method with thousands of them, which is exactly what the
		// lowering of a large method produces.
		size_t capacity = function->vregCount == 0 ? 64 : function->vregCount * 2;
		function->vregBank = realloc(function->vregBank, capacity);
		function->vregKind = realloc(function->vregKind, capacity);
		ASSERT(function->vregBank != NULL && function->vregKind != NULL);
	}
	uint32_t vreg = function->vregCount++;
	function->vregBank[vreg] = (uint8_t) bank;
	function->vregKind[vreg] = (uint8_t) kind;
	return vreg;
}


// ---- ordering and numbering -------------------------------------------------

// Post order, ITERATIVE. Recursion would be shorter and would also blow the C
// stack on a method with a few thousand blocks, which is the same reason
// jit/Passes.c collects its RPO this way; a lowering produces at least one LIR
// block per SSA block, so the bound here is no better than there.
//
// The successors are taken in REVERSE index order, so succs[0] finishes LAST
// among them and therefore comes FIRST in the reversed order. For a conditional
// branch succs[0] is the taken arm, and laying the taken arm next is what lets
// the emitter drop a jump for the common case.
static uint32_t collectPostOrder(LirFunction *function, LirBlock **post)
{
	_Bool *seen = calloc(function->blockCount, sizeof(_Bool));
	LirBlock **stack = calloc(function->blockCount + 1, sizeof(LirBlock *));
	uint8_t *visitIndex = calloc(function->blockCount, sizeof(uint8_t));
	ASSERT(seen != NULL && stack != NULL && visitIndex != NULL);

	uint32_t stackSize = 0, order = 0;
	stack[stackSize++] = function->entry;
	seen[function->entry->id] = 1;
	while (stackSize > 0) {
		LirBlock *block = stack[stackSize - 1];
		if (visitIndex[block->id] < block->succCount) {
			uint8_t taken = visitIndex[block->id]++;
			LirBlock *next = block->succs[block->succCount - 1 - taken];
			if (!seen[next->id]) {
				seen[next->id] = 1;
				stack[stackSize++] = next;
			}
		} else {
			post[order++] = block;
			stackSize--;
		}
	}

	free(seen);
	free(stack);
	free(visitIndex);
	return order;
}


void lirOrderAndNumber(LirFunction *function)
{
	free(function->order);
	function->order = calloc(function->blockCount, sizeof(LirBlock *));
	LirBlock **post = calloc(function->blockCount, sizeof(LirBlock *));
	ASSERT(function->order != NULL && post != NULL);

	uint32_t count = collectPostOrder(function, post);
	for (uint32_t i = 0; i < count; i++) {
		function->order[i] = post[count - 1 - i];
	}
	free(post);
	function->orderCount = count;

	// Relink the block list into layout order. UNREACHABLE BLOCKS ARE DROPPED
	// HERE, and that is deliberate rather than incidental: an unreachable block
	// has no position in the numbering, so an interval built from one would have
	// range -1..-1, and the allocator would compare it against real positions.
	// Dropping is also right on its own terms, since nothing can branch to it.
	function->blocks = NULL;
	LirBlock **tail = &function->blocks;
	for (uint32_t i = 0; i < count; i++) {
		function->order[i]->next = NULL;
		*tail = function->order[i];
		tail = &function->order[i]->next;
	}

	// EVEN positions for instructions, ODD for the gaps between them. The gap is
	// where a split lands and where a resolution move goes, so those never
	// collide with a real instruction's position, and a definition (which starts
	// at position + 1) is distinguishable from a use (which ends at position)
	// without either being a special case.
	int32_t position = 0;
	for (uint32_t i = 0; i < count; i++) {
		LirBlock *block = function->order[i];
		block->from = position;
		for (LirInstruction *instruction = block->first; instruction != NULL;
				instruction = instruction->next) {
			instruction->position = position;
			position += 2;
		}
		block->to = position;
	}
}


// ---- printing ---------------------------------------------------------------

static void printOperand(const LirFunction *function, uint32_t vreg, int16_t reg)
{
	if (vreg == LIR_NO_VREG) {
		printf(" -");
		return;
	}
	printf(" v%u%s", vreg,
		function->vregBank[vreg] == LIR_BANK_FLOAT ? "f" : "");
	if (reg != LIR_NO_REG) {
		printf("/r%d", reg);
	}
}


void lirPrint(const LirFunction *function)
{
	printf("lir: %u blocks, %u vregs, %u frame slots (%u outgoing)\n",
		function->blockCount, function->vregCount, function->frameSlots,
		function->outgoingSlots);
	for (const LirBlock *block = function->blocks; block != NULL;
			block = block->next) {
		printf("B%u (bci %u, %d..%d) preds:", block->id, block->label,
			block->from, block->to);
		for (uint16_t i = 0; i < block->predCount; i++) {
			printf(" B%u", block->preds[i]->id);
		}
		printf("\n");
		for (const LirInstruction *instruction = block->first;
				instruction != NULL; instruction = instruction->next) {
			printf("  %4d %-14s", instruction->position,
				lirOpName((LirOp) instruction->op));
			if (instruction->dst != LIR_NO_VREG) {
				printOperand(function, instruction->dst, instruction->dstReg);
				printf(" :=");
			}
			for (uint8_t i = 0; i < instruction->argCount; i++) {
				printOperand(function, instruction->args[i],
					instruction->argReg[i]);
			}
			if (instruction->imm != 0) {
				printf(" #%lld", (long long) instruction->imm);
			}
			if (instruction->disp != 0) {
				printf(" @%d", instruction->disp);
			}
			if (lirOpIsTerminator((LirOp) instruction->op)) {
				for (uint8_t i = 0; i < block->succCount; i++) {
					printf(" ->B%u", block->succs[i]->id);
				}
			}
			printf("\n");
		}
	}
}
