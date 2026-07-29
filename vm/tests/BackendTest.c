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
#include "jit/Deopt.h"
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
	// THREE DISTINCT IMMEDIATE CLASSES, which they were not until arithmetic
	// specialization needed them to be: all three tags pointed at SmallInteger,
	// so a guard demanding SmallInteger would have accepted a Character and a
	// float and nothing here would have noticed. The guard's whole job is to
	// tell those apart.
	Handles.Character.raw = classCreate(NULL, NULL, fixed0)->raw;
	Handles.SmallFloat64.raw = classCreate(NULL, NULL, fixed0)->raw;
	Handles.BoxedFloat64.raw = classCreate(NULL, NULL, bytes)->raw;

	Handles.symbolTable.raw = newArray(1024)->raw;
	// IMMORTAL: generated code bakes these three addresses as immediates.
	Handles.nil.raw = ((Object *) newImmortalObject(&Handles.UndefinedObject, 0))->raw;
	Handles.true_.raw = ((Object *) newImmortalObject(&Handles.True, 0))->raw;
	Handles.false_.raw = ((Object *) newImmortalObject(&Handles.False, 0))->raw;
	classSetImmediateIndices(classIndexOf(&Handles.SmallInteger),
		classIndexOf(&Handles.Character), classIndexOf(&Handles.SmallFloat64));
}


// The literal frame every unit built below is given. A file-static rather than
// a parameter, because the differential harness builds the two units itself and
// both tiers compile the SAME bytecode, so they need the same literals: a send
// takes its selector from literals[b] when the method is compiled.
static Value gTestLiterals;


static CodeUnit *makeUnit(Instruction *code, uint16_t count, uint16_t registers,
	uint16_t arguments)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->code = code;
	unit->instructionCount = count;
	unit->registerCount = registers;
	unit->argumentCount = arguments;
	unit->primitive = PRIM_NONE;
	unit->literals = gTestLiterals;
	return unit;
}

// Install `selector` on `class`, implemented by a method whose only content is
// `primitive`, exactly as packages/Core writes one.
//
// A REAL METHOD IN A REAL DICTIONARY, and not a hand-filled cache way. What the
// specialization bridge checks is the method the site RESOLVED TO -- its
// primitive number, reached through the cache's target -- so a test that filled
// the way in by hand would be testing the table and not the lookup that has to
// produce it.
static void installPrimitiveMethod(Class *class, const char *selector,
	uint16_t primitive, uint16_t arguments)
{
	static Instruction body[] = {
		{ OP_RET, 0, 0, 0, 0 },
	};
	CodeUnit *unit = makeUnit(body, 1, (uint16_t) (arguments + 2), arguments);
	unit->primitive = primitive;
	String *name = asSymbol(stringFromC(selector));
	CompiledMethod *method = compiledMethodCreate(unit, name, class);
	if (!valueTypeOf(class->raw->methodDictionary, VALUE_POINTER)) {
		class->raw->methodDictionary = objectTagged((Object *) newDictionary(8));
	}
	Dictionary *dictionary = scopeHandle(asObject(class->raw->methodDictionary));
	symbolDictAtPutObject(dictionary, name, (Object *) method);
}


static Value callTier(NativeCode *code, uint16_t arguments, Value receiver,
	Value *argv)
{
	switch (arguments) {
	case 0: return jitCall0(code, receiver);
	case 1: return jitCall1(code, receiver, argv[0]);
	default: return jitCall2(code, receiver, argv[0], argv[1]);
	}
}


// Compile one unit BOTH ways and compare what they answer.
//
// The unit is built fresh for each tier, because compiling registers the unit
// as a root and both would otherwise share one.
//
// `warmups` is how many times tier 1 runs BEFORE tier 2 is compiled, and it is
// the whole of what makes profile-driven specialization testable: the cells
// carry the profile, the cells belong to tier 1, and a method that has not run
// has nothing in them. `expectSpecialized` is what the optimizer must have done
// with what it found -- checked, not assumed, because the answer being right is
// also what an unspecialized compilation produces.
// The warm-up arguments and the measured arguments are SEPARATE, which is what
// lets a guard be made to fail on purpose: warm the site with two SmallIntegers
// so the optimizer speculates on them, then call with something else and check
// that leaving optimized code lands on the same answer tier 1 alone gives.
static void differentialWarm(const char *what, Instruction *code, uint16_t count,
	uint16_t registers, uint16_t arguments, Value warmReceiver, Value *warmArgv,
	Value receiver, Value *argv, int warmups, int expectSpecialized,
	int expectLeaves)
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

	for (int i = 0; i < warmups; i++) {
		callTier(baseline, arguments, warmReceiver, warmArgv);
	}

	// TIER 2 IS COMPILED BEFORE tier 1 is asked the question, and the order is
	// not cosmetic: running the measured call first would record ITS classes in
	// the very cells the compilation reads, so a test that deliberately calls
	// with an off-profile value would be handing the optimizer that value.
	const char *refused = NULL;
	PassStats stats;
	memset(&stats, 0, sizeof(stats));
	NativeCode *optimized = ssaCompile(ssaHostBackend(),
		makeUnit(code, count, registers, arguments), baseline, &refused, &stats);
	if (optimized == NULL) {
		gChecks++;
		// A REFUSAL IS A LEGITIMATE OUTCOME, and at a reduced pool it is the
		// EXPECTED one for a method that genuinely needs more registers than the
		// pool has: tier 1's code stands, which is always correct. Reporting it as
		// a failure would be reporting the tool as a defect.
		//
		// At the DEFAULT pool it is still a failure, because there it means the
		// method this check is about cannot be compiled at all, and every value
		// this function goes on to compare would be vacuous.
		if (getenv("ST_SSA_REGS") != NULL) {
			printf("  ok    %s (refused at a reduced pool: %s)\n", what, refused);
			return;
		}
		printf("  FAIL  %s: tier 2 refused (%s)\n", what, refused);
		gFailures++;
		return;
	}

	Value expected = callTier(baseline, arguments, receiver, argv);
	uint64_t before = jitDeoptimizationCount();
	Value actual = callTier(optimized, arguments, receiver, argv);
	uint64_t left = jitDeoptimizationCount() - before;

	gChecks++;
	if (expected != actual) {
		gFailures++;
		printf("  FAIL  %s: tier 1 answered 0x%llx, tier 2 answered 0x%llx\n",
			what, (unsigned long long) expected, (unsigned long long) actual);
		return;
	}
	if (expectSpecialized >= 0
			&& stats.sendsSpecialized != (uint32_t) expectSpecialized) {
		gFailures++;
		printf("  FAIL  %s: expected %d specialized send(s), got %u\n",
			what, expectSpecialized, stats.sendsSpecialized);
		return;
	}
	// AND WHETHER IT ACTUALLY LEFT, which the answer cannot say: deoptimizing
	// produces exactly what tier 1 produces, so a guard that fails on every call
	// passes every value check there is while delivering none of the speculation.
	// `expectLeaves` is negative for "do not care", 0 for "the speculation must
	// hold", 1 for "it must not".
	if (expectLeaves >= 0 && left != (uint64_t) expectLeaves) {
		gFailures++;
		printf("  FAIL  %s: expected %d deoptimization(s), got %llu\n",
			what, expectLeaves, (unsigned long long) left);
		return;
	}
	if (expectSpecialized > 0) {
		printf("  ok    %s (both answered 0x%llx, %u specialized, %llu left)\n",
			what, (unsigned long long) expected, stats.sendsSpecialized,
			(unsigned long long) left);
	} else {
		printf("  ok    %s (both answered 0x%llx)\n", what,
			(unsigned long long) expected);
	}
}


static void differential(const char *what, Instruction *code, uint16_t count,
	uint16_t registers, uint16_t arguments, Value receiver, Value *argv)
{
	differentialWarm(what, code, count, registers, arguments, receiver, argv,
		receiver, argv, 0, -1, -1);
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

	// ---- ARITHMETIC SPECIALIZATION, END TO END ------------------------------
	//
	// The one piece of tier 2 that can pay, and the one the whole deopt
	// machinery was built for. `^self + arg` is a SEND in the bytecode and stays
	// one in the IR (ADR 0006); what turns it into an addition is the PROFILE
	// that tier 1 accumulated by running it, and what makes that legal is the
	// guard, and what makes the guard legal is that failing it deoptimizes.
	//
	// Every case below is compared against tier 1 executing the same method, so
	// nothing here can be right for the wrong reason.
	printf("\n");
	installPrimitiveMethod(&Handles.SmallInteger, "+", PRIM_IntAdd, 1);
	installPrimitiveMethod(&Handles.SmallInteger, "<", PRIM_IntLessThan, 1);
	installPrimitiveMethod(&Handles.SmallFloat64, "+", PRIM_FloatAdd, 1);

	static Instruction binarySend[] = {
		{ OP_MOVE, 0, 3, 0, 0 },   // 0: r3 := self
		{ OP_MOVE, 0, 4, 1, 0 },   // 1: r4 := arg, one register above it
		{ OP_SEND, 1, 5, 0, 3 },   // 2: r5 := r3 <selector> r4
		{ OP_RET,  0, 5, 0, 0 },
	};
	// The literal frame has to hold the selector, because the site's cell takes
	// it from there when the method is compiled. One Array per selector, held in
	// this scope's handles so a collection during compilation moves them with
	// everything else.
	Value warm[2];
	Array *plusLiterals = newArray(1);
	arrayAtPutObject(plusLiterals, 0, (Object *) asSymbol(stringFromC("+")));
	Array *lessLiterals = newArray(1);
	arrayAtPutObject(lessLiterals, 0, (Object *) asSymbol(stringFromC("<")));
	gTestLiterals = objectTagged((Object *) plusLiterals);

	warm[0] = tagInt(4);
	arguments[0] = tagInt(4);
	differentialWarm("`self + arg` on two SmallIntegers", binarySend, 4, 8, 1,
		tagInt(3), warm, tagInt(3), arguments, 64, 1, 0);

	// A DIFFERENT PAIR through the SAME compiled code, so the addition is doing
	// arithmetic rather than replaying one warmed-up answer.
	arguments[0] = tagInt(-9);
	differentialWarm("and again with different values", binarySend, 4, 8, 1,
		tagInt(4), warm, tagInt(1000), arguments, 64, 1, 0);

	// ---- THE OVERFLOW, which is what the check on the addition is for -------
	//
	// SmallInteger maxVal plus one has no SmallInteger answer. The kernel's is a
	// LargeInteger built by the method's own Smalltalk, so the primitive FAILS
	// and the fallback runs -- here a fallback that answers the receiver, which
	// is a value tier 2 cannot produce by computing anything.
	//
	// Without the check on the addition this is not a slower answer, it is an
	// aborted process: the raw sum leaves the 62-bit payload and jitBoxInteger
	// asserts. Verified by removing the check and watching exactly that happen.
	arguments[0] = tagInt(1);
	differentialWarm("maxVal + 1 leaves optimized code", binarySend, 4, 8, 1,
		tagInt(3), warm, tagInt(((int64_t) 1 << 61) - 1), arguments, 64, 1, 1);

	// ---- THE OTHER TWO CHECKED OPERATIONS -----------------------------------
	//
	// `-` and `*` are specialized too, and the multiply is the only thing in the
	// system that reaches the overflow FLAG of the machine: an add or a subtract
	// of two 62-bit payloads cannot wrap 64 bits, so the range test alone answers
	// them, while a product can. Both of its failure paths are exercised here,
	// and without these the `jo` was emitted code nothing had ever executed.
	installPrimitiveMethod(&Handles.SmallInteger, "-", PRIM_IntSub, 1);
	installPrimitiveMethod(&Handles.SmallInteger, "*", PRIM_IntMul, 1);
	Array *minusLiterals = newArray(1);
	arrayAtPutObject(minusLiterals, 0, (Object *) asSymbol(stringFromC("-")));
	Array *timesLiterals = newArray(1);
	arrayAtPutObject(timesLiterals, 0, (Object *) asSymbol(stringFromC("*")));

	gTestLiterals = objectTagged((Object *) minusLiterals);
	warm[0] = tagInt(4);
	arguments[0] = tagInt(40);
	differentialWarm("`self - arg`", binarySend, 4, 8, 1,
		tagInt(9), warm, tagInt(7), arguments, 64, 1, 0);
	// minVal minus one, the other end of the ASYMMETRIC payload range.
	arguments[0] = tagInt(1);
	differentialWarm("minVal - 1 leaves optimized code", binarySend, 4, 8, 1,
		tagInt(9), warm, tagInt(-((int64_t) 1 << 61)), arguments, 64, 1, 1);

	gTestLiterals = objectTagged((Object *) timesLiterals);
	warm[0] = tagInt(3);
	arguments[0] = tagInt(-6);
	differentialWarm("`self * arg`", binarySend, 4, 8, 1,
		tagInt(7), warm, tagInt(7), arguments, 64, 1, 0);
	// A product that leaves 62 bits without leaving 64: only the range test sees
	// this one, and the machine's overflow flag never fires.
	arguments[0] = tagInt((int64_t) 1 << 31);
	differentialWarm("a product past 62 bits leaves", binarySend, 4, 8, 1,
		tagInt(7), warm, tagInt((int64_t) 1 << 31), arguments, 64, 1, 1);
	// And one that wraps 64 bits outright, which is the flag's own case: without
	// the `jo` the wrapped product lands back inside 62 bits and passes the range
	// test, so this is the check that separates the two halves.
	arguments[0] = tagInt((int64_t) 1 << 40);
	differentialWarm("a product that wraps 64 bits leaves", binarySend, 4, 8, 1,
		tagInt(7), warm, tagInt((int64_t) 1 << 40), arguments, 64, 1, 1);
	gTestLiterals = objectTagged((Object *) plusLiterals);

	// ---- THE GUARD FAILING, which is deoptimization on a real send ----------
	//
	// The site is warmed on two SmallIntegers and then handed a float argument.
	// The optimizer speculated; the speculation is wrong on this call; the guard
	// on the ARGUMENT fails, tier 1 re-executes the send from the bytecode index
	// the guard named, and the mixed-mode primitive answers a Float. Tier 1
	// alone answers the same, which is the entire claim.
	arguments[0] = tagFloat(0.5);
	differentialWarm("an off-profile argument deoptimizes", binarySend, 4, 8, 1,
		tagInt(3), warm, tagInt(3), arguments, 64, 1, 1);

	// ---- A COMPARISON, which answers a SINGLETON and not a number -----------
	//
	// `<` is the only relational primitive the kernel declares; Magnitude
	// derives the other three in Smalltalk. It exercises a different tail of the
	// specialization: icmp into bool2tag, so the answer is the `true` or `false`
	// object rather than a boxed number.
	gTestLiterals = objectTagged((Object *) lessLiterals);
	warm[0] = tagInt(10);
	arguments[0] = tagInt(10);
	differentialWarm("`self < arg` answering true", binarySend, 4, 8, 1,
		tagInt(2), warm, tagInt(2), arguments, 64, 1, 0);
	arguments[0] = tagInt(1);
	differentialWarm("`self < arg` answering false", binarySend, 4, 8, 1,
		tagInt(2), warm, tagInt(7), arguments, 64, 1, 0);

	// ---- AND THE FLOAT SIDE -------------------------------------------------
	//
	// A different bank, a different unbox, a different box. The float half of
	// the deopt save area only started existing for this case: a double living
	// in an XMM register had nowhere to be saved, so leaving here used to read
	// the integer register with the same number.
	gTestLiterals = objectTagged((Object *) plusLiterals);
	warm[0] = tagFloat(0.25);
	arguments[0] = tagFloat(0.25);
	differentialWarm("`self + arg` on two SmallFloat64s", binarySend, 4, 8, 1,
		tagFloat(1.5), warm, tagFloat(1.5), arguments, 64, 1, 0);

	// ---- A RAW DOUBLE LIVE WHEN OPTIMIZED CODE IS LEFT ----------------------
	//
	//   sum := self + step.
	//   [ sum := sum + step. count := count + 1. count < 5 ] whileTrue.
	//   ^sum
	//
	// The one shape that puts a raw double where deoptimization has to find it,
	// and finding it took a false start worth recording: a straight-line
	// `(a + b) < c` does NOT, because the comparison's receiver is the boxed sum,
	// so the bytecode register the state names holds the box and the raw value is
	// nobody's. Only PHI PROMOTION makes a bytecode register itself raw -- which
	// is why the accumulator is seeded by an addition here rather than by the
	// argument, so both of the phi's operands are boxes and promotion fires.
	//
	// Then the integer counter overflows, the checked addition leaves, and the
	// state it leaves with names a double living in a float register. That is
	// what the float half of the deopt save area is for; with the area covering
	// only the integer file the double is read out of the integer register with
	// the same number, which is a plausible and completely unrelated answer.
	//
	// Verified by removing the float saves and watching this one, and only this
	// one, answer differently.
	Array *bothLiterals = newArray(2);
	arrayAtPutObject(bothLiterals, 0, (Object *) asSymbol(stringFromC("+")));
	arrayAtPutObject(bothLiterals, 1, (Object *) asSymbol(stringFromC("<")));
	gTestLiterals = objectTagged((Object *) bothLiterals);

	static Instruction floatAccumulator[] = {
		{ OP_MOVE,     0, 4, 0, 0 },    //  0: r4 := self
		{ OP_MOVE,     0, 5, 1, 0 },    //  1: r5 := step
		{ OP_SEND,     1, 3, 0, 4 },    //  2: sum := self + step
		{ OP_MOVE,     0, 6, 2, 0 },    //  3: count := arg2
		{ OP_SAFEPOINT,0, 0, 0, 0 },    //  4: loop head
		{ OP_MOVE,     0, 7, 3, 0 },    //  5: r7 := sum
		{ OP_MOVE,     0, 8, 1, 0 },    //  6: r8 := step
		{ OP_SEND,     1, 3, 0, 7 },    //  7: sum := sum + step
		{ OP_MOVE,     0, 9, 6, 0 },    //  8: r9 := count
		{ OP_LOADI,    0, 10, 1, 0 },   //  9: r10 := 1
		{ OP_SEND,     1, 6, 0, 9 },    // 10: count := count + 1
		{ OP_MOVE,     0, 11, 6, 0 },   // 11: r11 := count
		{ OP_LOADI,    0, 12, 5, 0 },   // 12: r12 := 5
		{ OP_SEND,     1, 13, 1, 11 },  // 13: r13 := count < 5
		{ OP_JUMPTRUE, 0, 13, 4, 0 },   // 14: while it is, -> 4
		{ OP_RET,      0, 3, 0, 0 },    // 15: ^sum
	};
	Value pair[2];
	pair[0] = tagFloat(0.25);
	pair[1] = tagInt(0);
	arguments[0] = tagFloat(0.25);
	arguments[1] = tagInt(0);
	differentialWarm("a float accumulator loop, speculation holding",
		floatAccumulator, 16, 16, 2, tagFloat(1.5), pair, tagFloat(1.5),
		arguments, 64, 4, 0);

	// The same compiled code with the counter starting at maxVal, so the very
	// first `count + 1` overflows and leaves while the accumulator is raw.
	arguments[1] = tagInt(((int64_t) 1 << 61) - 1);
	differentialWarm("a raw double survives leaving optimized code",
		floatAccumulator, 16, 16, 2, tagFloat(1.5), pair, tagFloat(1.5),
		arguments, 64, 4, 1);
	gTestLiterals = objectTagged((Object *) plusLiterals);

	// ---- A COLD SITE IS NOT SPECIALIZED -------------------------------------
	//
	// The control, and the one that keeps every check above honest: the same
	// method with the same shape, compiled after ONE execution instead of 64,
	// has no evidence behind its dominant class and must stay a send. A bridge
	// that specialized on any profile at all would pass everything else here.
	arguments[0] = tagInt(4);
	differentialWarm("a cold site keeps its send", binarySend, 4, 8, 1,
		tagInt(3), warm, tagInt(3), arguments, 1, 0, 0);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
