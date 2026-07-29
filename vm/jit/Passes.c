#include "jit/Passes.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>

// The optimizer.
//
// THE ORDER MATTERS MORE THAN THE PASSES. Each one is ordinary; the sequence is
// what makes them compose:
//
//   1. trivial phis        clears the noise SSA construction leaves behind
//   2. type propagation    guards make types exact
//   3. redundant guards    a guard dominated by an equivalent one goes
//   4. scalar replacement  objects that do not escape become registers
//   5. GVN                 where box(unbox(x)) disappears
//   6. LICM                loop-invariant work leaves the loop
//   7. phi promotion       takes the boxing OFF the loop-carried edge
//   8. DCE                 removes the allocation that now has no users
//   9. block merging       makes the result readable
//
// Step 7 is the one most implementations omit, and it is the difference between
// a lukewarm result and a good one. An accumulator `total := total + x` is born
// as a TAGGED phi whose only producer is a box and whose only consumer is an
// unbox. Without promoting the phi to F64, boxing elimination stops at the loop
// boundary and every iteration pays for a box it immediately undoes.
//
// None of this is valid without the deoptimization maps. When step 4 erases an
// object, it leaves a MATERIALIZATION RECIPE in their place, so the object can
// be rebuilt at the moment a guard fails. That is what makes erasing it legal.


typedef struct {
	IrFunction *function;
	IrBlock **rpo;
	uint32_t rpoCount;
} PassContext;


static void collectRpo(PassContext *context)
{
	IrFunction *function = context->function;
	context->rpo = calloc(function->blockCount, sizeof(IrBlock *));
	_Bool *seen = calloc(function->blockCount, sizeof(_Bool));
	IrBlock **stack = calloc(function->blockCount + 1, sizeof(IrBlock *));
	uint32_t stackSize = 0, order = 0;

	// Iterative post-order, then reversed. Recursion would be shorter and would
	// also blow the C stack on a method with a few thousand blocks, which is
	// exactly the kind of method a bootstrap produces.
	IrBlock **post = calloc(function->blockCount, sizeof(IrBlock *));
	uint8_t *visitIndex = calloc(function->blockCount, sizeof(uint8_t));
	stack[stackSize++] = function->entry;
	seen[function->entry->id] = 1;
	while (stackSize > 0) {
		IrBlock *block = stack[stackSize - 1];
		if (visitIndex[block->id] < block->succCount) {
			IrBlock *next = block->succs[visitIndex[block->id]++];
			if (!seen[next->id]) {
				seen[next->id] = 1;
				stack[stackSize++] = next;
			}
		} else {
			post[order++] = block;
			stackSize--;
		}
	}
	for (uint32_t i = 0; i < order; i++) {
		context->rpo[i] = post[order - 1 - i];
	}
	context->rpoCount = order;
	free(seen);
	free(stack);
	free(post);
	free(visitIndex);
}


// ---------------------------------------------------------------------------
// 1. Trivial phis
// ---------------------------------------------------------------------------
//
// A phi is trivial when every operand is either the phi itself or one single
// other value. SSA construction produces these constantly: a register that is
// read inside a loop but never written there gets a phi whose operands are the
// entry definition and the phi itself.

static uint32_t removeTrivialPhis(PassContext *context)
{
	uint32_t removed = 0;
	_Bool changed = 1;
	while (changed) {
		changed = 0;
		for (IrBlock *block = context->function->blocks; block != NULL;
				block = block->next) {
			IrValue *phi = block->phis;
			while (phi != NULL) {
				IrValue *next = phi->next;
				IrValue *same = NULL;
				_Bool trivial = 1;
				for (uint16_t i = 0; i < phi->argCount; i++) {
					IrValue *arg = phi->args[i];
					if (arg == phi || arg == same) {
						continue;
					}
					if (same != NULL) {
						trivial = 0;
						break;
					}
					same = arg;
				}
				if (trivial && same != NULL) {
					irRemove(phi);
					irReplaceAllUses(context->function, phi, same);
					removed++;
					changed = 1;
				}
				phi = next;
			}
		}
	}
	return removed;
}


// ---------------------------------------------------------------------------
// 2. Type propagation
// ---------------------------------------------------------------------------
//
// Only UNCONDITIONAL knowledge is propagated here. A box produces its class by
// construction; a phi whose operands all agree produces that class. Guards do
// NOT write here, for the reason spelled out in Ir.h.

static uint32_t propagateTypes(PassContext *context)
{
	uint32_t learned = 0;
	_Bool changed = 1;
	while (changed) {
		changed = 0;
		for (uint32_t i = 0; i < context->rpoCount; i++) {
			IrBlock *block = context->rpo[i];
			for (IrValue *phi = block->phis; phi != NULL; phi = phi->next) {
				uint32_t agreed = CLASS_INDEX_INVALID;
				_Bool all = phi->argCount > 0;
				for (uint16_t a = 0; a < phi->argCount; a++) {
					uint32_t klass = phi->args[a] != NULL
						? phi->args[a]->klass : CLASS_INDEX_INVALID;
					if (klass == CLASS_INDEX_INVALID) { all = 0; break; }
					if (agreed == CLASS_INDEX_INVALID) { agreed = klass; }
					else if (agreed != klass) { all = 0; break; }
				}
				if (all && agreed != phi->klass) {
					phi->klass = agreed;
					learned++;
					changed = 1;
				}
			}
		}
	}
	return learned;
}


// ---------------------------------------------------------------------------
// 3. Redundant guards
// ---------------------------------------------------------------------------
//
// A guard is redundant when the class it checks is already known
// UNCONDITIONALLY, or when an earlier guard IN THE SAME BLOCK already
// established it. The second case builds its knowledge as it walks and never
// writes it onto the value, which is the difference between removing redundant
// guards and removing all of them.

static uint32_t removeRedundantGuards(PassContext *context)
{
	uint32_t removed = 0;
	uint32_t *established = calloc(context->function->valueCounter + 1,
		sizeof(uint32_t));
	for (uint32_t i = 0; i < context->rpoCount; i++) {
		IrBlock *block = context->rpo[i];
		memset(established, 0,
			(context->function->valueCounter + 1) * sizeof(uint32_t));
		IrValue *value = block->first;
		while (value != NULL) {
			IrValue *next = value->next;
			if (value->op == IR_GUARD_CLASS && value->argCount == 1) {
				IrValue *subject = value->args[0];
				uint32_t wanted = (uint32_t) value->extra;
				_Bool knownAlready = subject->klass == wanted
					|| established[subject->id] == wanted + 1;
				if (knownAlready) {
					irRemove(value);
					removed++;
				} else {
					// Recorded HERE, in the walk, and never on the value: see
					// the header of Ir.h for what writing it on the value does.
					established[subject->id] = wanted + 1;
				}
			}
			value = next;
		}
	}
	free(established);
	return removed;
}


// ---------------------------------------------------------------------------
// 3b. Local simplification, including the box/unbox pairs
// ---------------------------------------------------------------------------
//
// Not a numbered pass of its own: it is the local rewriting that scalar
// replacement and GVN both depend on. `fieldOf(new(...))` becoming the
// argument directly is what leaves an allocation with no users, and
// `unbox(box(x))` becoming `x` is what GVN then finds across blocks.
// ---------------------------------------------------------------------------

// Does anything between `from` and `to`, which share a block, write memory?
static _Bool writesBetween(IrValue *from, IrValue *to)
{
	for (IrValue *value = from->next; value != NULL && value != to;
			value = value->next) {
		if (irOpWritesMemory((IrOp) value->op)) {
			return 1;
		}
	}
	return 0;
}


static uint32_t simplify(PassContext *context)
{
	uint32_t rewritten = 0;
	for (uint32_t i = 0; i < context->rpoCount; i++) {
		IrBlock *block = context->rpo[i];
		IrValue *value = block->first;
		while (value != NULL) {
			IrValue *next = value->next;
			IrValue *source = value->argCount > 0 ? value->args[0] : NULL;
			IrValue *replacement = NULL;

			switch ((IrOp) value->op) {
			case IR_UNBOX_F:
				if (source != NULL && source->op == IR_BOX_F) { replacement = source->args[0]; }
				break;
			case IR_UNBOX_I:
				if (source != NULL && source->op == IR_BOX_I) { replacement = source->args[0]; }
				break;
			case IR_BOX_F:
				if (source != NULL && source->op == IR_UNBOX_F) { replacement = source->args[0]; }
				break;
			case IR_BOX_I:
				if (source != NULL && source->op == IR_UNBOX_I) { replacement = source->args[0]; }
				break;
			case IR_TAG2BOOL:
				if (source != NULL && source->op == IR_BOOL2TAG) { replacement = source->args[0]; }
				break;
			case IR_FIELD_F: case IR_FIELD_T:
				// Reading a field straight out of the allocation that filled
				// it. THIS is what makes scalar replacement work: once every
				// read is answered from the arguments, the allocation has no
				// users left and can go.
				//
				// ONLY WHEN NOTHING WROTE IN BETWEEN, which is the same trap GVN
				// was in: the allocation's argument is what the field held when
				// the object was BUILT, and a store between there and here means
				// that is no longer what it holds. Answered by walking the
				// instructions between the two rather than by a version counter,
				// because that also settles availability -- if the allocation is
				// not in this block, it is not walkable from here and the
				// substitution is simply not made.
				if (source != NULL && (source->op == IR_NEW || source->op == IR_NEWV)
						&& value->extra < source->argCount
						&& source->block == value->block
						&& !writesBetween(source, value)) {
					IrValue *field = source->args[value->extra];
					if (field->repr == value->repr) { replacement = field; }
				}
				break;
			default:
				break;
			}

			if (replacement != NULL) {
				irRemove(value);
				irReplaceAllUses(context->function, value, replacement);
				rewritten++;
			}
			value = next;
		}
	}
	return rewritten;
}


// ---------------------------------------------------------------------------
// 4. Scalar replacement, and the materialization recipe that makes it legal
// ---------------------------------------------------------------------------
//
// An object whose only remaining references are DEOPTIMIZATION STATES does not
// need to exist. Every field read has already been answered directly from the
// arguments of the allocation (that is what `simplify` does above), so nothing
// in the fast path can observe it.
//
// Erasing it is correct ONLY because of what is left in its place: a RECIPE
// naming the class and the values that fill the fields, so the object can be
// rebuilt at the exact moment a guard fails and the interpreter needs it. An
// escape analysis without a materialization recipe is not an optimization, it
// is a wrong answer waiting for a deoptimization, and --deopt-stress is what
// catches it.

// Does anything OTHER than a deopt state reference this value? A deopt state is
// a reference that costs nothing at run time, which is precisely why it does
// not force the object to exist.
static _Bool hasNonDeoptUsers(PassContext *context, IrValue *target)
{
	for (IrBlock *block = context->function->blocks; block != NULL;
			block = block->next) {
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			for (uint16_t i = 0; i < value->argCount; i++) {
				if (value->args[i] == target) { return 1; }
			}
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			for (uint16_t i = 0; i < value->argCount; i++) {
				if (value->args[i] == target) { return 1; }
			}
		}
		if (block->terminator != NULL) {
			for (uint16_t i = 0; i < block->terminator->argCount; i++) {
				if (block->terminator->args[i] == target) { return 1; }
			}
		}
	}
	return 0;
}


static void replaceWithRecipe(PassContext *context, IrValue *allocation,
	IrValue *recipeValue)
{
	for (IrBlock *block = context->function->blocks; block != NULL;
			block = block->next) {
		IrValue *values[3] = { block->phis, block->first, block->terminator };
		for (int list = 0; list < 3; list++) {
			for (IrValue *value = values[list]; value != NULL;
					value = list == 2 ? NULL : value->next) {
				if (value->deopt == NULL) { continue; }
				for (uint16_t f = 0; f < value->deopt->frameCount; f++) {
					DeoptFrame *frame = &value->deopt->frames[f];
					for (uint16_t sl = 0; sl < frame->slotCount; sl++) {
						if (frame->slotValue[sl] == allocation) {
							frame->slotValue[sl] = recipeValue;
						}
					}
				}
			}
		}
	}
}


static uint32_t scalarReplacement(PassContext *context, uint32_t *recipes)
{
	IrFunction *function = context->function;
	uint32_t replaced = 0;
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		IrValue *value = block->first;
		while (value != NULL) {
			IrValue *next = value->next;
			_Bool allocation = value->op == IR_NEW || value->op == IR_NEWV;
			if (allocation && !hasNonDeoptUsers(context, value)) {
				IrValue *recipeValue = irNewValue(function, IR_NEW);
				recipeValue->flags |= IR_FLAG_MATERIALIZE;
				Materialize *recipe = irAlloc(function, sizeof(Materialize));
				recipe->classIndex = (uint32_t) value->extra;
				recipe->flat = value->op == IR_NEWV;
				recipe->fieldCount = value->argCount;
				recipe->fields = irAlloc(function, value->argCount * sizeof(IrValue *));
				for (uint16_t i = 0; i < value->argCount; i++) {
					recipe->fields[i] = value->args[i];
				}
				recipeValue->recipe = recipe;
				replaceWithRecipe(context, value, recipeValue);
				irRemove(value);
				replaced++;
				(*recipes)++;
			}
			value = next;
		}
	}
	return replaced;
}


// ---------------------------------------------------------------------------
// 5. Global value numbering
// ---------------------------------------------------------------------------
//
// Over pure operations, in reverse post-order so a definition is numbered
// before its uses. This is where the box/unbox pairs that `simplify` did not
// catch locally collapse, and where repeated field loads and conversions
// become one.

// Defined with the loop passes below, and used here too: reusing a value is only
// legal when its definition DOMINATES the use.
static IrBlock **computeDominators(PassContext *context);
static _Bool dominates(IrBlock **idom, IrBlock *a, IrBlock *b);


// ---- what a store invalidates ----------------------------------------------
//
// PRECISE IN THE FIELD, CONSERVATIVE IN THE OBJECT. A store to field N kills
// reads of field N from anything, and does not touch reads of any other field.
// That is the cheapest split that is both sound and useful: proving two
// expressions name different OBJECTS is alias analysis and this optimizer has
// none, while the field index is sitting right there in the instruction.
//
// Everything else -- a send, a safepoint, an array store, a global store --
// bumps the ONE counter that every read depends on. A send runs arbitrary
// Smalltalk; a safepoint is a point execution can leave and another fiber can
// store at (ADR 0007); an array store is indexed by a runtime value, so nothing
// here can say which element it hit.
#define MEMORY_FIELD_SLOTS 64

typedef struct {
	uint32_t all;
	uint32_t field[MEMORY_FIELD_SLOTS];
} MemoryVersion;


static size_t memorySlot(int64_t fieldIndex)
{
	// A collision costs a missed reuse and nothing else, which is why a small
	// fixed table is enough for a field index that is a 16-bit operand.
	return (size_t) ((uint64_t) fieldIndex % MEMORY_FIELD_SLOTS);
}


static void memoryAdvance(MemoryVersion *version, IrValue *value)
{
	switch ((IrOp) value->op) {
	case IR_SETFIELD_T: case IR_SETFIELD_F:
		version->field[memorySlot(value->extra)]++;
		break;
	case IR_ASTORE: case IR_VASTORE:
	case IR_SETGLOBAL: case IR_SEND: case IR_SAFEPOINT:
		version->all++;
		break;
	default:
		break;
	}
}


// The field counter a given read depends on, or NULL when it depends only on
// `all` (an array load or a length).
static uint32_t memoryFieldVersion(const MemoryVersion *version, IrValue *read)
{
	if (read->op == IR_FIELD_T || read->op == IR_FIELD_F) {
		return version->field[memorySlot(read->extra)];
	}
	return 0;
}


typedef struct {
	IrValue *value;
	uint32_t hash;
	// The two counters this entry was recorded at. Only meaningful for a memory
	// read, and together they are what stops one being reused across a store
	// that could have changed it.
	uint32_t memoryAll;
	uint32_t memoryField;
} GvnEntry;


static uint32_t gvnHash(IrValue *value, const MemoryVersion *memory)
{
	uint32_t hash = (uint32_t) value->op * 2654435761u;
	hash ^= (uint32_t) value->extra * 40503u;
	hash ^= (uint32_t) value->konst;
	for (uint16_t i = 0; i < value->argCount; i++) {
		hash = hash * 31u + (value->args[i] != NULL ? value->args[i]->id : 0);
	}
	if (irOpReadsMemory((IrOp) value->op)) {
		hash = hash * 31u + memory->all;
		hash = hash * 31u + memoryFieldVersion(memory, value);
	}
	return hash;
}


static _Bool gvnEqual(IrValue *a, IrValue *b)
{
	if (a->op != b->op || a->extra != b->extra || a->konst != b->konst
			|| a->argCount != b->argCount || a->repr != b->repr) {
		return 0;
	}
	if (a->op == IR_FCONST && a->fkonst != b->fkonst) { return 0; }
	if (a->op == IR_ICONST && a->ikonst != b->ikonst) { return 0; }
	for (uint16_t i = 0; i < a->argCount; i++) {
		if (a->args[i] != b->args[i]) { return 0; }
	}
	return 1;
}


static uint32_t globalValueNumbering(PassContext *context)
{
	uint32_t removed = 0;
	size_t capacity = 1024;
	GvnEntry *table = calloc(capacity, sizeof(GvnEntry));
	IrBlock **idom = computeDominators(context);
	// Advanced by anything that may write. A memory read carries the counters it
	// depends on in its key, so an identical read on the other side of a store
	// that could have changed it is a DIFFERENT value.
	MemoryVersion memory;
	memset(&memory, 0, sizeof(memory));

	for (uint32_t i = 0; i < context->rpoCount; i++) {
		IrBlock *block = context->rpo[i];
		IrValue *value = block->first;
		while (value != NULL) {
			IrValue *next = value->next;
			memoryAdvance(&memory, value);
			// Allocations are pure but must NOT be numbered together: two
			// `new` instructions produce two distinct objects, and collapsing
			// them would give one identity where the program expects two.
			_Bool numberable = irOpIsPure((IrOp) value->op)
				&& value->op != IR_NEW && value->op != IR_NEWV
				&& value->op != IR_ANEW && value->op != IR_VNEW
				&& value->op != IR_PARAM && value->op != IR_PHI;
			if (numberable) {
				uint32_t hash = gvnHash(value, &memory);
				size_t slot = hash % capacity;
				IrValue *found = NULL;
				for (size_t probe = 0; probe < capacity; probe++) {
					if (table[slot].value == NULL) { break; }
					if (table[slot].hash == hash
							&& gvnEqual(table[slot].value, value)
							// SAME HEAP. Equal operands are not enough for a
							// memory read: the earlier one describes the heap as
							// it was at ITS version.
							&& (!irOpReadsMemory((IrOp) value->op)
								|| (table[slot].memoryAll == memory.all
									&& table[slot].memoryField
										== memoryFieldVersion(&memory, value)))
							// AND IT HAS TO DOMINATE. Reusing a value from a
							// block that merely ran EARLIER in reverse post
							// order is not the same thing as reusing one that is
							// guaranteed to have run: two arms of a conditional
							// are ordered by the walk and neither reaches the
							// other, so the replacement would read a register
							// that was never written on the path taken.
							&& dominates(idom, table[slot].value->block, block)) {
						found = table[slot].value;
						break;
					}
					slot = (slot + 1) % capacity;
				}
				if (found != NULL) {
					irRemove(value);
					irReplaceAllUses(context->function, value, found);
					removed++;
				} else {
					table[slot].value = value;
					table[slot].hash = hash;
					table[slot].memoryAll = memory.all;
					table[slot].memoryField = memoryFieldVersion(&memory, value);
				}
			}
			value = next;
		}
	}
	free(idom);
	free(table);
	return removed;
}


// ---------------------------------------------------------------------------
// 6. Loop-invariant code motion
// ---------------------------------------------------------------------------
//
// Without this, a field read like `data` is performed once per iteration and
// the promise of "no tagged access inside the loop" does not survive contact
// with reality. Applied repeatedly it lifts work out of an inner loop into an
// outer one and then out of the outer one entirely.
//
// Only PURE operations move, and only when every operand is defined outside
// the loop. A guard does NOT move: hoisting a guard out of a loop changes WHEN
// it fails, and its deoptimization state describes a bytecode index inside the
// loop, so the state would resume at a point the program had not reached. Doing
// it properly needs either rewriting the state for the preheader or versioning
// the loop, and pretending otherwise would be the sort of thing that looks like
// an optimization and is a wrong answer.

// Immediate dominators by the iterative algorithm, over reverse post-order.
static IrBlock **computeDominators(PassContext *context)
{
	IrFunction *function = context->function;
	IrBlock **idom = calloc(function->blockCount, sizeof(IrBlock *));
	uint32_t *position = calloc(function->blockCount, sizeof(uint32_t));
	for (uint32_t i = 0; i < context->rpoCount; i++) {
		position[context->rpo[i]->id] = i;
	}
	idom[function->entry->id] = function->entry;

	_Bool changed = 1;
	while (changed) {
		changed = 0;
		for (uint32_t i = 0; i < context->rpoCount; i++) {
			IrBlock *block = context->rpo[i];
			if (block == function->entry) {
				continue;
			}
			IrBlock *candidate = NULL;
			for (uint16_t p = 0; p < block->predCount; p++) {
				IrBlock *pred = block->preds[p];
				if (idom[pred->id] == NULL) {
					continue;
				}
				if (candidate == NULL) {
					candidate = pred;
					continue;
				}
				// Walk both up the dominator tree until they meet.
				IrBlock *a = candidate;
				IrBlock *b = pred;
				while (a != b) {
					while (position[a->id] > position[b->id]) { a = idom[a->id]; }
					while (position[b->id] > position[a->id]) { b = idom[b->id]; }
				}
				candidate = a;
			}
			if (candidate != NULL && idom[block->id] != candidate) {
				idom[block->id] = candidate;
				changed = 1;
			}
		}
	}
	free(position);
	return idom;
}


static _Bool dominates(IrBlock **idom, IrBlock *a, IrBlock *b)
{
	IrBlock *cursor = b;
	for (;;) {
		if (cursor == a) { return 1; }
		IrBlock *next = idom[cursor->id];
		if (next == NULL || next == cursor) { return 0; }
		cursor = next;
	}
}


static uint32_t hoistLoopInvariants(PassContext *context)
{
	IrFunction *function = context->function;
	IrBlock **idom = computeDominators(context);
	uint32_t hoisted = 0;

	for (uint32_t i = 0; i < context->rpoCount; i++) {
		IrBlock *latch = context->rpo[i];
		for (uint8_t s = 0; s < latch->succCount; s++) {
			IrBlock *header = latch->succs[s];
			// A BACK EDGE: an edge to a block that dominates its source.
			if (!dominates(idom, header, latch)) {
				continue;
			}
			// The body is everything that reaches the latch without leaving
			// through the header.
			_Bool *inLoop = calloc(function->blockCount, sizeof(_Bool));
			IrBlock **stack = calloc(function->blockCount, sizeof(IrBlock *));
			uint32_t stackSize = 0;
			inLoop[header->id] = 1;
			if (!inLoop[latch->id]) {
				inLoop[latch->id] = 1;
				stack[stackSize++] = latch;
			}
			while (stackSize > 0) {
				IrBlock *block = stack[--stackSize];
				for (uint16_t p = 0; p < block->predCount; p++) {
					IrBlock *pred = block->preds[p];
					if (!inLoop[pred->id]) {
						inLoop[pred->id] = 1;
						stack[stackSize++] = pred;
					}
				}
			}

			// The preheader: the single predecessor of the header from OUTSIDE.
			// With more than one there is nowhere unambiguous to hoist to, and
			// inventing a block would change the CFG under the deopt states.
			IrBlock *preheader = NULL;
			uint16_t outside = 0;
			for (uint16_t p = 0; p < header->predCount; p++) {
				if (!inLoop[header->preds[p]->id]) {
					preheader = header->preds[p];
					outside++;
				}
			}
			if (outside != 1 || preheader == NULL) {
				free(inLoop);
				free(stack);
				continue;
			}

			// WHAT THE LOOP WRITES. A memory read is pure and still cannot leave
			// a loop that stores to what it reads: hoisted, it would answer the
			// first iteration's value for every iteration.
			//
			// Same split as GVN, and the same reason: the field index is known,
			// the object is not. A loop that writes field 3 does not stop a read
			// of field 2 from being hoisted, which is exactly the case the
			// existing check below exercises.
			MemoryVersion loopWrites;
			memset(&loopWrites, 0, sizeof(loopWrites));
			for (uint32_t b = 0; b < context->rpoCount; b++) {
				IrBlock *block = context->rpo[b];
				if (!inLoop[block->id]) {
					continue;
				}
				for (IrValue *value = block->first; value != NULL;
						value = value->next) {
					memoryAdvance(&loopWrites, value);
				}
			}

			for (uint32_t b = 0; b < context->rpoCount; b++) {
				IrBlock *block = context->rpo[b];
				if (!inLoop[block->id]) {
					continue;
				}
				IrValue *value = block->first;
				while (value != NULL) {
					IrValue *next = value->next;
					_Bool movable = irOpIsPure((IrOp) value->op)
						&& value->op != IR_PHI && value->op != IR_PARAM
						&& value->deopt == NULL;
					if (movable && irOpReadsMemory((IrOp) value->op)) {
						movable = loopWrites.all == 0
							&& memoryFieldVersion(&loopWrites, value) == 0;
					}
					for (uint16_t k = 0; k < value->argCount && movable; k++) {
						IrValue *arg = value->args[k];
						// An operand defined INSIDE the loop makes the value
						// vary with the iteration.
						movable = arg == NULL || arg->block == NULL
							|| !inLoop[arg->block->id];
					}
					if (movable) {
						irRemove(value);
						irAppend(preheader, value);
						hoisted++;
					}
					value = next;
				}
			}
			free(inLoop);
			free(stack);
		}
	}
	free(idom);
	return hoisted;
}


// ---------------------------------------------------------------------------
// 8. Dead code
// ---------------------------------------------------------------------------
//
// Mark from the effectful operations and from the DEOPTIMIZATION STATES, then
// sweep. Forgetting the states is how a value that is only needed when a guard
// fails gets deleted, and the result is a wrong answer on exactly the path
// nobody exercises.

static void markLive(IrValue *value, _Bool *live, IrValue **worklist,
	uint32_t *worklistSize)
{
	if (value == NULL || live[value->id]) {
		return;
	}
	live[value->id] = 1;
	worklist[(*worklistSize)++] = value;
}


static uint32_t eliminateDeadCode(PassContext *context)
{
	IrFunction *function = context->function;
	_Bool *live = calloc(function->valueCounter + 1, sizeof(_Bool));
	IrValue **worklist = calloc(function->valueCounter + 1, sizeof(IrValue *));
	uint32_t worklistSize = 0;

	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			if (!irOpIsPure((IrOp) value->op)) {
				markLive(value, live, worklist, &worklistSize);
			}
		}
		markLive(block->terminator, live, worklist, &worklistSize);
	}

	while (worklistSize > 0) {
		IrValue *value = worklist[--worklistSize];
		for (uint16_t i = 0; i < value->argCount; i++) {
			markLive(value->args[i], live, worklist, &worklistSize);
		}
		if (value->deopt != NULL) {
			for (uint16_t f = 0; f < value->deopt->frameCount; f++) {
				DeoptFrame *frame = &value->deopt->frames[f];
				for (uint16_t s = 0; s < frame->slotCount; s++) {
					IrValue *slot = frame->slotValue[s];
					if (slot != NULL && (slot->flags & IR_FLAG_MATERIALIZE)) {
						Materialize *recipe = slot->recipe;
						for (uint16_t k = 0; k < recipe->fieldCount; k++) {
							markLive(recipe->fields[k], live, worklist, &worklistSize);
						}
					} else {
						markLive(slot, live, worklist, &worklistSize);
					}
				}
			}
		}
	}

	uint32_t removed = 0;
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		IrValue *value = block->first;
		while (value != NULL) {
			IrValue *next = value->next;
			if (!live[value->id]) {
				irRemove(value);
				removed++;
			}
			value = next;
		}
		IrValue *phi = block->phis;
		while (phi != NULL) {
			IrValue *next = phi->next;
			if (!live[phi->id]) {
				irRemove(phi);
				removed++;
			}
			phi = next;
		}
	}
	free(live);
	free(worklist);
	return removed;
}


// ---------------------------------------------------------------------------
// 7. Representation promotion of loop-carried phis
// ---------------------------------------------------------------------------
//
// Written below dead code in this file and RUN BEFORE it in the pipeline: the
// order that matters is the one in irOptimize, not the order of definitions.
//
// The pass most implementations leave out, and the one that decides whether the
// result is lukewarm or good.
//
// An accumulator is born as a TAGGED phi whose only producers are boxes and
// whose only consumers are unboxes. Promoting the phi to F64 makes both
// disappear and the accumulator lives in a register for the whole loop. Without
// it, every iteration pays for a box it immediately undoes, and all the boxing
// elimination downstream stops dead at the loop boundary.

// After a phi changes representation, every USE of it has to be made consistent
// again. Two cases, and missing the second is a wrong answer rather than a
// missed optimization.
static void fixPromotedConsumers(PassContext *context, IrValue *phi,
	IrOp unboxOp, IrOp boxOp)
{
	IrFunction *function = context->function;
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		IrValue *value = block->first;
		while (value != NULL) {
			IrValue *next = value->next;
			_Bool usesPhi = 0;
			for (uint16_t i = 0; i < value->argCount; i++) {
				usesPhi = usesPhi || value->args[i] == phi;
			}
			if (usesPhi && value->op == unboxOp) {
				// The unbox of an already-raw value is the identity.
				irRemove(value);
				irReplaceAllUses(function, value, phi);
			} else if (usesPhi) {
				for (uint16_t i = 0; i < value->argCount; i++) {
					if (value->args[i] == phi
							&& !irOperandIsRaw((IrOp) value->op, i)
							&& value->op != IR_PHI) {
						IrValue *box = irNewValue(function, boxOp);
						irAddArg(function, box, phi);
						irInsertBefore(block, value, box);
						value->args[i] = box;
					}
				}
			}
			value = next;
		}
		// The terminator too: `ret` and `branch` both take tagged values, and
		// forgetting them is how a promoted accumulator gets returned as a
		// bit pattern.
		IrValue *terminator = block->terminator;
		if (terminator != NULL) {
			for (uint16_t i = 0; i < terminator->argCount; i++) {
				if (terminator->args[i] == phi
						&& !irOperandIsRaw((IrOp) terminator->op, i)) {
					IrValue *box = irNewValue(function, boxOp);
					irAddArg(function, box, phi);
					irAppend(block, box);
					terminator->args[i] = box;
				}
			}
		}
	}
}


static uint32_t promotePhis(PassContext *context)
{
	uint32_t promoted = 0;
	for (IrBlock *block = context->function->blocks; block != NULL;
			block = block->next) {
		for (IrValue *phi = block->phis; phi != NULL; phi = phi->next) {
			if (phi->repr != REPR_TAGGED || phi->argCount == 0) {
				continue;
			}
			// Every operand must be a box of the SAME representation, or the
			// phi itself. Mixed producers mean the value genuinely has to be
			// tagged somewhere, and promoting would just move the conversion.
			IrOp boxOp = IR_OP_COUNT;
			_Bool uniform = 1;
			for (uint16_t i = 0; i < phi->argCount && uniform; i++) {
				IrValue *arg = phi->args[i];
				if (arg == phi) { continue; }
				if (arg->op != IR_BOX_F && arg->op != IR_BOX_I) { uniform = 0; break; }
				if (boxOp == IR_OP_COUNT) { boxOp = (IrOp) arg->op; }
				else if (boxOp != arg->op) { uniform = 0; }
			}
			if (!uniform || boxOp == IR_OP_COUNT) {
				continue;
			}
			phi->repr = boxOp == IR_BOX_F ? REPR_F64 : REPR_I64;
			for (uint16_t i = 0; i < phi->argCount; i++) {
				if (phi->args[i] != phi) {
					phi->args[i] = phi->args[i]->args[0]; // through the box
				}
			}
			// AND THE CONSUMERS. Rewiring only the producers is not merely
			// incomplete, it is WRONG: every use that expects a tagged value
			// would now be reading a raw double. The matching unbox becomes the
			// phi itself; anything that genuinely needs a tagged value gets a
			// box inserted right before it.
			fixPromotedConsumers(context, phi,
				boxOp == IR_BOX_F ? IR_UNBOX_F : IR_UNBOX_I, boxOp);
			promoted++;
		}
	}
	return promoted;
}


// ---------------------------------------------------------------------------
// 9. Block merging
// ---------------------------------------------------------------------------

static uint32_t mergeBlocks(PassContext *context)
{
	uint32_t merged = 0;
	for (IrBlock *block = context->function->blocks; block != NULL;
			block = block->next) {
		if (block->succCount != 1 || block->terminator == NULL
				|| block->terminator->op != IR_JUMP) {
			continue;
		}
		IrBlock *successor = block->succs[0];
		if (successor == block || successor->predCount != 1
				|| successor->phis != NULL) {
			continue;
		}
		for (IrValue *value = successor->first; value != NULL; value = value->next) {
			value->block = block;
		}
		if (successor->first != NULL) {
			if (block->last == NULL) {
				block->first = successor->first;
			} else {
				block->last->next = successor->first;
			}
			block->last = successor->last;
		}
		block->terminator = successor->terminator;
		if (block->terminator != NULL) {
			block->terminator->block = block;
		}
		block->succCount = successor->succCount;
		for (uint8_t s = 0; s < successor->succCount; s++) {
			block->succs[s] = successor->succs[s];
			for (uint16_t p = 0; p < successor->succs[s]->predCount; p++) {
				if (successor->succs[s]->preds[p] == successor) {
					successor->succs[s]->preds[p] = block;
				}
			}
		}
		successor->first = successor->last = NULL;
		successor->terminator = NULL;
		successor->succCount = 0;
		merged++;
	}
	return merged;
}


// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

PassStats irOptimize(IrFunction *function)
{
	PassStats stats;
	memset(&stats, 0, sizeof(stats));

	PassContext context;
	context.function = function;
	collectRpo(&context);

	stats.trivialPhis += removeTrivialPhis(&context);
	stats.typesLearned += propagateTypes(&context);

	// Fixed point: simplification exposes GVN opportunities and GVN exposes
	// more simplification, and both expose dead code.
	for (int round = 0; round < 6; round++) {
		uint32_t before = stats.simplified + stats.gvnRemoved + stats.guardsRemoved;
		stats.guardsRemoved += removeRedundantGuards(&context);
		stats.simplified += simplify(&context);
		// AFTER simplify, which is what turns field reads into direct uses of
		// the allocation's arguments and therefore what leaves the allocation
		// with no non-deopt users to begin with.
		stats.scalarReplaced += scalarReplacement(&context, &stats.materializations);
		stats.gvnRemoved += globalValueNumbering(&context);
		stats.typesLearned += propagateTypes(&context);
		stats.trivialPhis += removeTrivialPhis(&context);
		if (stats.simplified + stats.gvnRemoved + stats.guardsRemoved == before) {
			break;
		}
	}

	// LICM before promotion: hoisting can expose a phi whose producers become
	// uniform once the invariant part has left the loop.
	stats.hoisted += hoistLoopInvariants(&context);

	// Promotion goes AFTER the fixed point and BEFORE the final cleanup: it
	// needs the boxes to have settled into their final shape, and it creates a
	// new round of removable conversions.
	stats.phisPromoted += promotePhis(&context);
	for (int round = 0; round < 3; round++) {
		stats.simplified += simplify(&context);
		stats.gvnRemoved += globalValueNumbering(&context);
		stats.trivialPhis += removeTrivialPhis(&context);
	}

	stats.deadRemoved += eliminateDeadCode(&context);
	stats.blocksMerged += mergeBlocks(&context);

	free(context.rpo);
	return stats;
}
