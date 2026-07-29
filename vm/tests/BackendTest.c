// The SSA backend: allocated LIR becomes machine code that RUNS.
//
// THE ORACLE IS TIER 1, and that is the point of the whole file. Every method
// here is compiled TWICE, by the template compiler and by the SSA backend, and
// both are executed and their answers compared. Under a dry cut (ADR 0002) the
// external oracle is gone and the internal one, --deopt-stress, does not exist
// yet; this is the one that does exist, and it is available now precisely
// because tier 1 is a complete, independently proved implementation of the same
// language.
//
// What a differential test catches that a value check does not: the answer
// being right for the wrong reason. A method that answers 42 because the
// backend constant-folded it and a method that answers 42 because it ran are
// indistinguishable to `check(result == 42)` and distinguishable here only
// because tier 1 arrives at 42 by a completely different route.
//
// AND IT RUNS AT EVERY REGISTER-POOL SIZE. ST_SSA_REGS shrinks the allocator's
// pool, so the same methods go through the spill and split paths that a
// twelve-register file never reaches. Those paths are the ones least likely to
// have been executed and most likely to be wrong.

#include "compiler/Bytecode.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "jit/InlineCache.h"
#include "jit/MacroAssembler.h"
#include "jit/SsaBackend.h"
#include "memory/Collector.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

__thread Thread CurrentThread;
ptrdiff_t gCurrentThreadTpoff;

static int gFailures;
static int gChecks;


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


static void bootstrapMinimal(Heap *heap)
{
	bootstrapClassOfClasses(heap);
	InstanceShape bytes = DEFINE_SHAPE(FORMAT_BYTES, 0, 0, 0);
	InstanceShape array = DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, 0);
	InstanceShape fixed0 = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 0);
	InstanceShape fixed2 = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 2);

	Handles.String.raw = classCreate(NULL, NULL, bytes)->raw;
	Handles.Symbol.raw = classCreate(&Handles.String, NULL, bytes)->raw;
	Handles.Array.raw = classCreate(NULL, NULL, array)->raw;
	Handles.Association.raw = classCreate(NULL, NULL, fixed2)->raw;
	Handles.Dictionary.raw = classCreate(NULL, NULL, fixed2)->raw;
	Handles.OrderedCollection.raw = classCreate(NULL, NULL,
		(InstanceShape) DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 3))->raw;
	Handles.CompiledMethod.raw = classCreate(NULL, NULL, COMPILED_METHOD_SHAPE)->raw;
	Handles.UndefinedObject.raw = classCreate(NULL, NULL, fixed0)->raw;
	Handles.True.raw = classCreate(NULL, NULL, fixed0)->raw;
	Handles.False.raw = classCreate(NULL, NULL, fixed0)->raw;
	Handles.SmallInteger.raw = classCreate(NULL, NULL, fixed0)->raw;

	Handles.symbolTable.raw = newArray(1024)->raw;
	// IMMORTAL: generated code bakes these three addresses as immediates.
	Handles.nil.raw = ((Object *) newImmortalObject(&Handles.UndefinedObject, 0))->raw;
	Handles.true_.raw = ((Object *) newImmortalObject(&Handles.True, 0))->raw;
	Handles.false_.raw = ((Object *) newImmortalObject(&Handles.False, 0))->raw;
	classSetImmediateIndices(classIndexOf(&Handles.SmallInteger),
		classIndexOf(&Handles.SmallInteger), classIndexOf(&Handles.SmallInteger));
}


static CodeUnit *makeUnit(Instruction *code, uint16_t count, uint16_t registers,
	uint16_t arguments)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->code = code;
	unit->instructionCount = count;
	unit->registerCount = registers;
	unit->argumentCount = arguments;
	unit->primitive = PRIM_NONE;
	return unit;
}


// Compile one unit BOTH ways and compare what they answer.
//
// The unit is built fresh for each tier, because compiling registers the unit
// as a root and both would otherwise share one.
static void differential(const char *what, Instruction *code, uint16_t count,
	uint16_t registers, uint16_t arguments, Value receiver, Value *argv)
{
	Opcode unsupported = OP_COUNT;
	NativeCode *baseline = jitCompile(makeUnit(code, count, registers, arguments),
		&unsupported);
	if (baseline == NULL) {
		printf("  FAIL  %s: tier 1 refused (%s)\n", what,
			opcodeName(unsupported));
		gChecks++;
		gFailures++;
		return;
	}

	const char *refused = NULL;
	NativeCode *optimized = ssaCompile(ssaHostBackend(),
		makeUnit(code, count, registers, arguments), baseline, &refused);
	if (optimized == NULL) {
		printf("  FAIL  %s: tier 2 refused (%s)\n", what, refused);
		gChecks++;
		gFailures++;
		return;
	}

	Value expected, actual;
	switch (arguments) {
	case 0:
		expected = jitCall0(baseline, receiver);
		actual = jitCall0(optimized, receiver);
		break;
	case 1:
		expected = jitCall1(baseline, receiver, argv[0]);
		actual = jitCall1(optimized, receiver, argv[0]);
		break;
	default:
		expected = jitCall2(baseline, receiver, argv[0], argv[1]);
		actual = jitCall2(optimized, receiver, argv[0], argv[1]);
		break;
	}

	gChecks++;
	if (expected != actual) {
		gFailures++;
		printf("  FAIL  %s: tier 1 answered 0x%llx, tier 2 answered 0x%llx\n",
			what, (unsigned long long) expected, (unsigned long long) actual);
	} else {
		printf("  ok    %s (both answered 0x%llx)\n", what,
			(unsigned long long) expected);
	}
}


int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
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

	const char *pool = getenv("ST_SSA_REGS");
	printf("the SSA backend emits machine code that runs (pool %s)\n\n",
		pool != NULL ? pool : "default");

	HandleScope outer;
	openHandleScope(&outer);
	bootstrapMinimal(&heap);

	Value nil = tagPtr(Handles.nil.raw);
	Value arguments[2];

	// ---- a constant ---------------------------------------------------------
	static Instruction constant[] = {
		{ OP_LOADI, 0, 1, 42, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	differential("^42", constant, 2, 2, 0, nil, NULL);

	// ---- a negative literal, where sign extension goes wrong ----------------
	static Instruction negative[] = {
		{ OP_LOADI, 0, 1, (uint16_t) -1234, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	differential("^-1234", negative, 2, 2, 0, nil, NULL);

	// ---- the receiver, which proves the prologue wrote slot 0 ---------------
	static Instruction self[] = {
		{ OP_RET, 0, 0, 0, 0 },
	};
	differential("^self", self, 1, 2, 0, tagInt(7), NULL);

	// ---- an argument, which proves the parameter slots line up --------------
	static Instruction firstArgument[] = {
		{ OP_RET, 0, 1, 0, 0 },
	};
	arguments[0] = tagInt(99);
	differential("^arg1", firstArgument, 1, 3, 1, nil, arguments);

	static Instruction secondArgument[] = {
		{ OP_RET, 0, 2, 0, 0 },
	};
	arguments[0] = tagInt(11);
	arguments[1] = tagInt(22);
	differential("^arg2", secondArgument, 1, 4, 2, nil, arguments);

	// ---- nil, true and false, the three baked singletons --------------------
	static Instruction singletons[] = {
		{ OP_LOADTRUE, 0, 1, 0, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	differential("^true", singletons, 2, 2, 0, nil, NULL);

	// ---- a conditional, which is a compare against a baked singleton --------
	//   ^arg1 ifTrue: [1] ifFalse: [2]   in its emitted shape
	static Instruction conditional[] = {
		{ OP_JUMPFALSE, 0, 1, 3, 0 },
		{ OP_LOADI, 0, 2, 1, 0 },
		{ OP_RET, 0, 2, 0, 0 },
		{ OP_LOADI, 0, 2, 2, 0 },
		{ OP_RET, 0, 2, 0, 0 },
	};
	arguments[0] = tagPtr(Handles.true_.raw);
	differential("a taken branch", conditional, 5, 3, 1, nil, arguments);
	arguments[0] = tagPtr(Handles.false_.raw);
	differential("the other branch", conditional, 5, 3, 1, nil, arguments);

	// ---- a BACKWARD branch and a phi ----------------------------------------
	//
	// The one the label machinery gets wrong, and the one that makes the
	// allocator build an interval spanning a back edge.
	static Instruction loop[] = {
		{ OP_LOADI, 0, 1, 0, 0 },        // 0: counter := 0
		{ OP_SAFEPOINT, 0, 0, 0, 0 },    // 1: loop head
		{ OP_JUMPTRUE, 0, 2, 4, 0 },     // 2: if arg -> 4
		{ OP_JUMP, 0, 1, 0, 0 },         // 3: -> 1
		{ OP_RET, 0, 1, 0, 0 },          // 4
	};
	// r2 is the SECOND argument and it is what the loop tests, so it has to be
	// set: leaving it holding whatever the previous case left there is an
	// infinite loop, and both tiers spin identically, which is the differential
	// harness reporting the truth about a broken test rather than a broken
	// backend. What is being proved is the EDGE, not the iteration count.
	arguments[0] = tagInt(0);
	arguments[1] = tagPtr(Handles.true_.raw);
	differential("a back edge", loop, 5, 4, 2, nil, arguments);

	// ---- many live values at once, which is what makes the allocator spill --
	static Instruction pressure[] = {
		{ OP_LOADI, 0, 3, 1, 0 },
		{ OP_LOADI, 0, 4, 2, 0 },
		{ OP_LOADI, 0, 5, 3, 0 },
		{ OP_LOADI, 0, 6, 4, 0 },
		{ OP_LOADI, 0, 7, 5, 0 },
		{ OP_LOADI, 0, 8, 6, 0 },
		{ OP_LOADI, 0, 9, 7, 0 },
		{ OP_LOADI, 0, 10, 8, 0 },
		{ OP_LOADI, 0, 11, 9, 0 },
		{ OP_LOADI, 0, 12, 10, 0 },
		{ OP_MOVE, 0, 13, 3, 0 },
		{ OP_MOVE, 0, 14, 12, 0 },
		{ OP_RET, 0, 14, 0, 0 },
	};
	differential("ten live values", pressure, 13, 16, 0, nil, NULL);

	// ---- A GUARD THAT FAILS, which is deoptimization end to end -------------
	//
	// The receiver is a SmallInteger and the guard demands a pointer class, so
	// the speculation is wrong on every call and the method finishes in tier 1
	// from the bytecode index the guard named. What is checked is the ANSWER:
	// tier 1 alone and tier 2 deoptimizing into tier 1 have to agree, which is
	// the whole claim --deopt-stress will make over the entire suite.
	//
	// The guard's target is a class no receiver here can have, so it fails
	// deterministically rather than depending on what the heap happened to lay
	// out.
	static Instruction guarded[] = {
		{ OP_LOADI, 0, 2, 7, 0 },        // 0: r2 := 7
		{ OP_GUARDCLASS, 0, 0, 0, 2 },   // 1: guard r0 is class 0 -> else 2
		{ OP_RET, 0, 2, 0, 0 },          // 2
	};
	// Class index 0 belongs to no ordinary object, so the guard cannot hold.
	differential("a guard that always fails", guarded, 3, 4, 0, tagInt(5), NULL);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
