// Gate level 6: escape analysis, and the materialization that makes it legal.
//
// This is phase 3 of the plan, and its ordering is the point. The rule the
// plan states is that the deoptimization machinery is built BEFORE the
// optimizer, not after, and the reason is exactly what this file tests:
//
//   erasing an object is correct ONLY if it can be rebuilt at the moment a
//   guard fails.
//
// An escape analysis without a materialization recipe is not an optimization
// that is a bit too aggressive. It is a wrong answer waiting for a
// deoptimization, and the path it goes wrong on is the one nobody exercises.
//
// So the test does the whole round trip: build an object, let the optimizer
// prove nothing in the fast path can observe it, watch it disappear, then
// REBUILD it from the recipe and check that every field came back.

#include "core/Class.h"
#include "core/Handle.h"
#include "jit/Deopt.h"
#include "jit/Ir.h"
#include "jit/Passes.h"
#include "memory/Collector.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "runtime/Collection.h"
#include "runtime/String.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__thread Thread CurrentThread;
ptrdiff_t gCurrentThreadTpoff;

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


static void bootstrapClassOfClasses(Heap *heap)
{
	size_t bytes = objectSizeForShape(CLASS_OF_CLASSES_SHAPE, CLASS_RAW_TRAILER_BYTES);
	RawClass *class = (RawClass *) allocate(heap, bytes);
	uint32_t index = classTableAdd(&heap->classes, (RawObject *) class);
	class->header = makeObjectHeader(index, index, FORMAT_MIXED_BYTES,
		bytes / sizeof(uint64_t));
	class->instanceShape = CLASS_OF_CLASSES_SHAPE;
	class->classIndex = index;
	Handles.ClassClass.raw = class;
}


static CodeUnit *emptyUnit(uint16_t registers)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->registerCount = registers;
	return unit;
}


static IrValue *emitInto(IrFunction *function, IrBlock *block, IrOp op)
{
	IrValue *value = irNewValue(function, op);
	irAppend(block, value);
	return value;
}


static uint32_t countOp(IrFunction *function, IrOp op)
{
	uint32_t count = 0;
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			count += value->op == op && !(value->flags & IR_FLAG_MATERIALIZE);
		}
	}
	return count;
}


int main(void)
{
	Heap heap;
	CurrentThread.tlab.top = NULL;
	CurrentThread.tlab.end = NULL;
	CurrentThread.handleScopes = NULL;
	initRememberedSet(&CurrentThread.rememberedSet);
	initHeap(&heap, &CurrentThread);
	CurrentThread.heap = &heap;
	heap.mutators = &CurrentThread;
	CurrentThread.nextMutator = NULL;
	initHandles();

	printf("gate level 6: escape analysis and materialization\n\n");

	HandleScope outer;
	openHandleScope(&outer);
	bootstrapClassOfClasses(&heap);
	InstanceShape three = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 3);
	Class *point = classCreate(NULL, NULL, three);
	uint32_t pointClass = classIndexOf(point);

	// ---- an object that does not escape ------------------------------------
	//
	//   p := Point x: a y: b z: c.
	//   ^p x                                (only the field is used)
	//
	// A guard sits between the allocation and the read, and the guard's deopt
	// state names `p`, which is what makes this the interesting case: the fast
	// path does not need the object, but the SLOW path does.
	IrFunction *function = irCreate(emptyUnit(4));
	IrBlock *entry = irNewBlock(function, 0);
	function->entry = entry;

	IrValue *a = emitInto(function, entry, IR_PARAM);
	IrValue *b = emitInto(function, entry, IR_PARAM);
	b->extra = 1;
	IrValue *c = emitInto(function, entry, IR_PARAM);
	c->extra = 2;

	IrValue *allocation = emitInto(function, entry, IR_NEW);
	allocation->extra = pointClass;
	allocation->klass = pointClass;
	irAddArg(function, allocation, a);
	irAddArg(function, allocation, b);
	irAddArg(function, allocation, c);

	// A guard whose deopt state holds the object. Frame layout: one frame,
	// innermost, one live slot holding the allocation.
	IrValue *guard = emitInto(function, entry, IR_GUARD_CLASS);
	irAddArg(function, guard, a);
	guard->extra = 12345;
	DeoptState *state = irAlloc(function, sizeof(DeoptState));
	state->frames = irAlloc(function, sizeof(DeoptFrame));
	state->frameCount = 1;
	state->frames[0].unit = function->unit;
	state->frames[0].bci = 3;
	state->frames[0].innermost = 1;
	state->frames[0].destRegister = BYTECODE_NO_TARGET;
	state->frames[0].slotCount = 1;
	state->frames[0].slotRegister = irAlloc(function, sizeof(uint16_t));
	state->frames[0].slotValue = irAlloc(function, sizeof(IrValue *));
	state->frames[0].slotRegister[0] = 3;
	state->frames[0].slotValue[0] = allocation;
	guard->deopt = state;

	IrValue *read = emitInto(function, entry, IR_FIELD_T);
	irAddArg(function, read, allocation);
	read->extra = 0; // the x field
	entry->terminator = irNewValue(function, IR_RET);
	irAddArg(function, entry->terminator, read);
	entry->terminator->block = entry;

	check("before: the object is allocated", countOp(function, IR_NEW) == 1);
	check("before: the deopt state is well formed",
		deoptStateIsWellFormed(state));

	PassStats stats = irOptimize(function);
	printf("\n  passes: simplificados=%u  escalarizados=%u  receitas=%u  mortos=%u\n\n",
		stats.simplified, stats.scalarReplaced, stats.materializations,
		stats.deadRemoved);

	check("the field read was answered from the allocation's argument",
		stats.simplified >= 1);
	check("and with nothing left observing it, the ALLOCATION IS GONE",
		countOp(function, IR_NEW) == 0);
	check("one materialization recipe was left behind",
		stats.materializations == 1);

	IrValue *slot = state->frames[0].slotValue[0];
	check("the deopt state now names a RECIPE, not the erased allocation",
		slot != NULL && (slot->flags & IR_FLAG_MATERIALIZE));
	check("the recipe knows the class", slot->recipe->classIndex == pointClass);
	check("and names one value per field",
		slot->recipe->fieldCount == 3
		&& slot->recipe->fields[0] == a
		&& slot->recipe->fields[1] == b
		&& slot->recipe->fields[2] == c);
	check("the state is STILL well formed after the erasure",
		deoptStateIsWellFormed(state));

	// ---- and now rebuild it ------------------------------------------------
	//
	// This is the moment a guard fails. The values that would have gone into
	// the object are read out of the optimized frame; here they are supplied
	// directly, which is the same thing without a tier-2 backend to read from.
	Value fields[3] = { tagInt(11), tagInt(22), tagInt(33) };
	Value rebuilt = deoptMaterialize(slot->recipe->classIndex, slot->recipe->flat,
		fields, slot->recipe->fieldCount);
	check("materializing answers an object", valueTypeOf(rebuilt, VALUE_POINTER));
	check("of the recipe's class",
		rawObjectClassIndex(asObject(rebuilt)) == pointClass);
	check("with every field back where it belongs",
		((Value *) asObject(rebuilt)->body)[0] == tagInt(11)
		&& ((Value *) asObject(rebuilt)->body)[1] == tagInt(22)
		&& ((Value *) asObject(rebuilt)->body)[2] == tagInt(33));

	// Materializing ALLOCATES, so it can collect, with the whole deopt state
	// live. Rebuilding many objects in a row is the shape that finds out.
	Value survivors[64];
	for (int i = 0; i < 64; i++) {
		Value f[3] = { tagInt(i), tagInt(i * 2), tagInt(i * 3) };
		survivors[i] = deoptMaterialize(pointClass, 0, f, 3);
	}
	collectorSetExtraRoots(survivors, 64);
	collectorScavenge(&heap);
	_Bool allIntact = 1;
	for (int i = 0; i < 64; i++) {
		Value *body = (Value *) asObject(survivors[i])->body;
		allIntact = allIntact && body[0] == tagInt(i) && body[1] == tagInt(i * 2)
			&& body[2] == tagInt(i * 3);
	}
	check("many materializations in a row survive a collection intact", allIntact);

	// ---- an object that DOES escape must not be erased ---------------------
	IrFunction *escaping = irCreate(emptyUnit(4));
	IrBlock *e0 = irNewBlock(escaping, 0);
	escaping->entry = e0;
	IrValue *ea = emitInto(escaping, e0, IR_PARAM);
	IrValue *kept = emitInto(escaping, e0, IR_NEW);
	kept->extra = pointClass;
	irAddArg(escaping, kept, ea);
	// Returned, so it is observable and erasing it would be wrong.
	e0->terminator = irNewValue(escaping, IR_RET);
	irAddArg(escaping, e0->terminator, kept);
	e0->terminator->block = e0;

	PassStats escapeStats = irOptimize(escaping);
	check("an object that ESCAPES is not erased",
		countOp(escaping, IR_NEW) == 1 && escapeStats.scalarReplaced == 0);

	check("--deopt-stress is off unless asked for", !deoptStressEnabled());

	closeHandleScope(&outer, NULL);
	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
