#include "jit/Ir.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Arena chunks. The IR lives exactly as long as one compilation, so nothing is
// ever individually freed and a removed instruction can stay referenced by a
// deopt state with no ownership question to answer.
typedef struct IrArenaChunk {
	struct IrArenaChunk *next;
	size_t used, capacity;
	uint8_t bytes[];
} IrArenaChunk;

#define ARENA_CHUNK_BYTES (64 * 1024)


static const struct {
	const char *name;
	Repr repr;
	_Bool pure;
} gOps[IR_OP_COUNT] = {
	[IR_CONST]     = { "const",     REPR_TAGGED, 1 },
	// PURE, and it is a load: the unit's literal frame is an Array the collector
	// moves, so the backend reaches an element through the address of the unit's
	// own field. What makes it pure anyway is that the CONTENTS never change --
	// a literal frame is written once, when the method is compiled -- so the
	// value read is stable even though the address it was read from is not.
	[IR_LITERAL]   = { "literal",   REPR_TAGGED, 1 },
	[IR_FCONST]    = { "fconst",    REPR_F64,    1 },
	[IR_ICONST]    = { "iconst",    REPR_I64,    1 },
	[IR_PARAM]     = { "param",     REPR_TAGGED, 1 },

	[IR_BOX_F]     = { "box_f",     REPR_TAGGED, 1 },
	[IR_BOX_I]     = { "box_i",     REPR_TAGGED, 1 },
	[IR_BOOL2TAG]  = { "bool2tag",  REPR_TAGGED, 1 },
	[IR_UNBOX_F]   = { "unbox_f",   REPR_F64,    1 },
	[IR_UNBOX_I]   = { "unbox_i",   REPR_I64,    1 },
	[IR_TAG2BOOL]  = { "tag2bool",  REPR_BOOL,   1 },
	[IR_I2F]       = { "i2f",       REPR_F64,    1 },
	[IR_F2I]       = { "f2i",       REPR_I64,    1 },

	[IR_FADD]      = { "fadd",      REPR_F64,    1 },
	[IR_FSUB]      = { "fsub",      REPR_F64,    1 },
	[IR_FMUL]      = { "fmul",      REPR_F64,    1 },
	[IR_FDIV]      = { "fdiv",      REPR_F64,    1 },
	[IR_FNEG]      = { "fneg",      REPR_F64,    1 },
	[IR_FSQRT]     = { "fsqrt",     REPR_F64,    1 },
	[IR_IADD]      = { "iadd",      REPR_I64,    1 },
	[IR_ISUB]      = { "isub",      REPR_I64,    1 },
	[IR_IMUL]      = { "imul",      REPR_I64,    1 },
	[IR_IDIV]      = { "idiv",      REPR_I64,    1 },
	[IR_IMOD]      = { "imod",      REPR_I64,    1 },
	[IR_FCMP]      = { "fcmp",      REPR_BOOL,   1 },
	[IR_ICMP]      = { "icmp",      REPR_BOOL,   1 },

	[IR_FIELD_T]   = { "fieldt",    REPR_TAGGED, 1 },
	[IR_FIELD_F]   = { "fieldf",    REPR_F64,    1 },
	[IR_SETFIELD_T]= { "setfieldt", REPR_VOID,   0 },
	[IR_SETFIELD_F]= { "setfieldf", REPR_VOID,   0 },
	[IR_GLOBAL]    = { "global",    REPR_TAGGED, 0 },
	[IR_SETGLOBAL] = { "setglobal", REPR_VOID,   0 },
	[IR_ALOAD]     = { "aload",     REPR_TAGGED, 1 },
	[IR_ASTORE]    = { "astore",    REPR_VOID,   0 },
	[IR_ALEN]      = { "alen",      REPR_I64,    1 },
	[IR_VALOAD]    = { "vaload",    REPR_F64,    1 },
	[IR_VASTORE]   = { "vastore",   REPR_VOID,   0 },
	[IR_VALEN]     = { "valen",     REPR_I64,    1 },

	// Allocations are PURE in the sense the optimizer cares about: they read no
	// mutable state, so a dead one may be deleted. That is exactly what makes
	// scalar replacement expressible, and it is safe only because deletion
	// leaves a materialization recipe behind.
	[IR_NEW]       = { "new",       REPR_TAGGED, 1 },
	[IR_NEWV]      = { "newv",      REPR_TAGGED, 1 },
	[IR_ANEW]      = { "anew",      REPR_TAGGED, 1 },
	[IR_VNEW]      = { "vnew",      REPR_TAGGED, 1 },
	[IR_NEWCELL]   = { "newcell",   REPR_TAGGED, 1 },
	[IR_CLOSURE]   = { "closure",   REPR_TAGGED, 1 },

	[IR_SEND]      = { "send",      REPR_TAGGED, 0 },
	[IR_GUARD_CLASS]={ "guard_class",REPR_VOID,  0 },
	[IR_SAFEPOINT] = { "safepoint", REPR_VOID,   0 },
	[IR_RET]       = { "ret",       REPR_VOID,   0 },
	[IR_JUMP]      = { "jump",      REPR_VOID,   0 },
	[IR_BRANCH]    = { "branch",    REPR_VOID,   0 },
	[IR_RETOUTER]  = { "retouter",  REPR_VOID,   0 },
	[IR_PHI]       = { "phi",       REPR_TAGGED, 1 },
};


// Reads mutable heap memory. Two of these are the same value only if nothing
// wrote in between, which is the fact `pure` does not carry.
_Bool irOpReadsMemory(IrOp op)
{
	switch (op) {
	case IR_FIELD_T: case IR_FIELD_F:
	case IR_ALOAD: case IR_VALOAD:
	// The lengths are conservative: an array's length cannot change, so these
	// two could be memoryless. Listed anyway, because the cost is one missed
	// redundancy on a load nobody has measured and the alternative is a
	// correctness argument carried in a comment.
	case IR_ALEN: case IR_VALEN:
		return 1;
	default:
		return 0;
	}
}


// Writes memory, or might.
//
// IR_SEND is the one that matters: a residual dispatch runs arbitrary Smalltalk,
// so every memory read after it is a different read from every one before it.
// IR_SAFEPOINT is here for a less obvious reason -- execution can LEAVE at one
// (ADR 0007) and another fiber can run and store, so a read cannot be carried
// across one either.
_Bool irOpWritesMemory(IrOp op)
{
	switch (op) {
	case IR_SETFIELD_T: case IR_SETFIELD_F:
	case IR_ASTORE: case IR_VASTORE:
	case IR_SETGLOBAL:
	case IR_SEND:
	case IR_SAFEPOINT:
		return 1;
	default:
		return 0;
	}
}


Repr irOpRepr(IrOp op) { return gOps[op].repr; }
_Bool irOpIsPure(IrOp op) { return gOps[op].pure; }
const char *irOpName(IrOp op) { return gOps[op].name; }


_Bool irOpIsTerminator(IrOp op)
{
	return op == IR_RET || op == IR_JUMP || op == IR_BRANCH
		|| op == IR_RETOUTER;
}


_Bool irValueCanDeoptimize(const IrValue *value)
{
	return value->op == IR_GUARD_CLASS
		|| (value->flags & IR_FLAG_CHECK_OVERFLOW) != 0;
}


void *irAlloc(IrFunction *function, size_t bytes)
{
	bytes = (bytes + 15) & ~(size_t) 15;
	IrArenaChunk *chunk = function->chunks;
	if (chunk == NULL || chunk->used + bytes > chunk->capacity) {
		size_t capacity = bytes > ARENA_CHUNK_BYTES ? bytes : ARENA_CHUNK_BYTES;
		chunk = malloc(sizeof(IrArenaChunk) + capacity);
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


IrFunction *irCreate(CodeUnit *unit)
{
	IrFunction *function = calloc(1, sizeof(IrFunction));
	ASSERT(function != NULL);
	function->unit = unit;
	function->registerCount = unit->registerCount;
	return function;
}


void irDestroy(IrFunction *function)
{
	IrArenaChunk *chunk = function->chunks;
	while (chunk != NULL) {
		IrArenaChunk *next = chunk->next;
		free(chunk);
		chunk = next;
	}
	free(function);
}


IrBlock *irNewBlock(IrFunction *function, uint16_t label)
{
	IrBlock *block = irAlloc(function, sizeof(IrBlock));
	block->id = function->blockCount++;
	block->label = label;
	block->defs = irAlloc(function, function->registerCount * sizeof(IrValue *));
	block->incompletePhis = irAlloc(function,
		function->registerCount * sizeof(IrValue *));
	block->next = function->blocks;
	function->blocks = block;
	return block;
}


IrValue *irNewValue(IrFunction *function, IrOp op)
{
	IrValue *value = irAlloc(function, sizeof(IrValue));
	value->op = (uint16_t) op;
	value->repr = (uint8_t) irOpRepr(op);
	value->id = function->valueCounter++;
	value->klass = CLASS_INDEX_INVALID;
	// Not zero, which is a perfectly ordinary bytecode index: an op that has no
	// site has to be distinguishable from one whose site is the first
	// instruction, or a backend reading it finds the cache cell of bci 0.
	value->bci = BYTECODE_NO_TARGET;
	return value;
}


void irAddArg(IrFunction *function, IrValue *value, IrValue *arg)
{
	if (value->argCount == value->argCapacity) {
		uint16_t capacity = value->argCapacity == 0 ? 4
			: (uint16_t) (value->argCapacity * 2);
		IrValue **args = irAlloc(function, capacity * sizeof(IrValue *));
		if (value->args != NULL) {
			memcpy(args, value->args, value->argCount * sizeof(IrValue *));
		}
		value->args = args;
		value->argCapacity = capacity;
	}
	value->args[value->argCount++] = arg;
}


void irAppend(IrBlock *block, IrValue *value)
{
	value->block = block;
	value->next = NULL;
	if (value->op == IR_PHI) {
		value->next = block->phis;
		block->phis = value;
		return;
	}
	if (block->last == NULL) {
		block->first = block->last = value;
	} else {
		block->last->next = value;
		block->last = value;
	}
}


void irRemove(IrValue *value)
{
	IrBlock *block = value->block;
	if (block == NULL) {
		return;
	}
	if (value->op == IR_PHI) {
		IrValue **link = &block->phis;
		while (*link != NULL && *link != value) {
			link = &(*link)->next;
		}
		if (*link == value) {
			*link = value->next;
		}
	} else {
		IrValue **link = &block->first;
		IrValue *previous = NULL;
		while (*link != NULL && *link != value) {
			previous = *link;
			link = &(*link)->next;
		}
		if (*link == value) {
			*link = value->next;
			if (block->last == value) {
				block->last = previous;
			}
		}
	}
	value->block = NULL;
}


static void replaceInDeopt(DeoptState *state, IrValue *from, IrValue *to)
{
	if (state == NULL) {
		return;
	}
	for (uint16_t f = 0; f < state->frameCount; f++) {
		DeoptFrame *frame = &state->frames[f];
		for (uint16_t s = 0; s < frame->slotCount; s++) {
			IrValue *slot = frame->slotValue[s];
			if (slot == from) {
				frame->slotValue[s] = to;
			} else if (slot != NULL && (slot->flags & IR_FLAG_MATERIALIZE)) {
				Materialize *recipe = (Materialize *) slot->args;
				for (uint16_t i = 0; i < recipe->fieldCount; i++) {
					if (recipe->fields[i] == from) {
						recipe->fields[i] = to;
					}
				}
			}
		}
	}
}


void irReplaceAllUses(IrFunction *function, IrValue *from, IrValue *to)
{
	if (from == to) {
		return;
	}
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			for (uint16_t i = 0; i < value->argCount; i++) {
				if (value->args[i] == from) { value->args[i] = to; }
			}
			replaceInDeopt(value->deopt, from, to);
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			for (uint16_t i = 0; i < value->argCount; i++) {
				if (value->args[i] == from) { value->args[i] = to; }
			}
			// Deopt states MUST be rewritten too. A value that survives only
			// inside a deopt state is exactly the case that goes wrong when a
			// guard fails, which is the hardest place to notice a stale
			// reference.
			replaceInDeopt(value->deopt, from, to);
		}
		if (block->terminator != NULL) {
			IrValue *value = block->terminator;
			for (uint16_t i = 0; i < value->argCount; i++) {
				if (value->args[i] == from) { value->args[i] = to; }
			}
			replaceInDeopt(value->deopt, from, to);
		}
	}
}


// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------

static void printValue(IrValue *value)
{
	static const char *reprName[] = { "void", "tagged", "f64", "i64", "bool" };
	printf("    v%u:%-6s = %-11s", value->id, reprName[value->repr],
		irOpName((IrOp) value->op));
	if (value->op == IR_ICONST) {
		printf(" %lld", (long long) value->ikonst);
	} else if (value->op == IR_FCONST) {
		printf(" %g", value->fkonst);
	} else if (value->op == IR_CONST) {
		printf(" 0x%llx", (unsigned long long) value->konst);
	}
	if (value->op == IR_GUARD_CLASS || value->op == IR_NEW || value->op == IR_NEWV
			|| value->op == IR_FIELD_T || value->op == IR_FIELD_F
			|| value->op == IR_PARAM || value->op == IR_LITERAL
			|| value->op == IR_GLOBAL || value->op == IR_SETGLOBAL) {
		printf(" {%lld}", (long long) value->extra);
	}
	if (value->argCount > 0) {
		printf(" (");
		for (uint16_t i = 0; i < value->argCount; i++) {
			printf("%sv%u", i > 0 ? ", " : "",
				value->args[i] != NULL ? value->args[i]->id : 0);
		}
		printf(")");
	}
	if (value->klass != CLASS_INDEX_INVALID) {
		printf("  cls=%u", value->klass);
	}
	if (value->deopt != NULL) {
		printf("  deopt[%u]", value->deopt->frameCount);
	}
	printf("\n");
}


void irPrint(IrFunction *function)
{
	// Blocks are pushed onto the front of the list, so print them in creation
	// order to read like the bytecode did.
	uint32_t count = function->blockCount;
	IrBlock **ordered = calloc(count, sizeof(IrBlock *));
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		ordered[block->id] = block;
	}
	for (uint32_t i = 0; i < count; i++) {
		IrBlock *block = ordered[i];
		if (block == NULL) {
			continue;
		}
		printf("  B%u (bci %u), preds:", block->id, block->label);
		for (uint16_t p = 0; p < block->predCount; p++) {
			printf(" B%u", block->preds[p]->id);
		}
		printf("\n");
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			printValue(value);
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			printValue(value);
		}
		if (block->terminator != NULL) {
			printValue(block->terminator);
		}
	}
	free(ordered);
}


void irInsertBefore(IrBlock *block, IrValue *before, IrValue *value)
{
	value->block = block;
	if (before == NULL || block->first == NULL) {
		irAppend(block, value);
		return;
	}
	if (block->first == before) {
		value->next = block->first;
		block->first = value;
		return;
	}
	IrValue *previous = block->first;
	while (previous->next != NULL && previous->next != before) {
		previous = previous->next;
	}
	value->next = previous->next;
	previous->next = value;
	if (block->last == previous && value->next == NULL) {
		block->last = value;
	}
}


_Bool irOperandIsRaw(IrOp op, uint16_t index)
{
	switch (op) {
	// Raw arithmetic and conversions consume raw values.
	case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
	case IR_FNEG: case IR_FSQRT: case IR_FCMP:
	case IR_IADD: case IR_ISUB: case IR_IMUL: case IR_IDIV: case IR_IMOD:
	case IR_ICMP: case IR_I2F: case IR_F2I:
	case IR_BOX_F: case IR_BOX_I:
		return 1;
	// Indexed access takes a tagged receiver and a RAW index.
	case IR_ALOAD: case IR_VALOAD:
		return index == 1;
	case IR_ASTORE:
		return index == 1;
	case IR_VASTORE:
		return index >= 1;
	// A raw field store takes a tagged object and a raw value.
	case IR_SETFIELD_F:
		return index == 1;
	default:
		return 0;
	}
}
