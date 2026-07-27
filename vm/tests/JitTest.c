// Gate level 3: the JIT emits machine code that runs.
//
// Hand-written bytecode, compiled to x86-64, executed. No parser and no
// front end: the JIT's input is bytecode, so writing the bytecode by hand is
// what lets it be proved on its own, before anything can produce any.
//
// What each check is really testing:
//
//   * the ENCODINGS. Every instruction the assembler can emit is emitted here
//     and executed, so a wrong ModRM byte or a missing REX prefix is a wrong
//     answer or a crash rather than something that sits unnoticed;
//   * the FRAME CONTRACT. Slot i is bytecode register i at [rbp - 8(i+1)],
//     with no allocator and no per-compile variation. That invariant is what
//     makes a deoptimization map writable at all, so it is worth proving with
//     arguments that have to survive being stored and reloaded;
//   * the BACKWARD BRANCH, which is the one the label machinery gets wrong;
//   * the SEND path end to end: dispatch by class index, method lookup up the
//     superclass chain, compile on first call, enter, return.

#include "compiler/Bytecode.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "jit/InlineCache.h"
#include "jit/MacroAssembler.h"
#include "memory/Collector.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"
#include <stdio.h>
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


static Class *defineClass(uint16_t instanceVariables)
{
	InstanceShape shape = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, instanceVariables);
	Class *class = classCreate(NULL, NULL, shape);
	Dictionary *methods = newDictionary(8);
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->methodDictionary,
		(RawObject *) methods->raw);
	return class;
}


static void defineMethod(Class *class, const char *selector, CodeUnit *unit)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *name = asSymbol(stringFromC(selector));
	CompiledMethod *method = compiledMethodCreate(unit, name, class);
	Dictionary *methods = scopeHandle(asObject(class->raw->methodDictionary));
	symbolDictAtPutObject(methods, name, (Object *) method);
	closeHandleScope(&scope, NULL);
}


// A unit built from a static instruction array. Literals are an Array so a SEND
// can name its selector the way a real compiled method does.
static CodeUnit *makeUnit(Instruction *code, uint16_t count, uint16_t registers,
	uint16_t arguments, Array *literals)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->code = code;
	unit->instructionCount = count;
	unit->registerCount = registers;
	unit->argumentCount = arguments;
	unit->literals = literals != NULL ? tagPtr(literals->raw) : 0;
	return unit;
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
	// IMMORTAL: generated code bakes these three addresses as immediates, so
	// they must never move. jitCompileFor asserts it on every compilation.
	Handles.nil.raw = ((Object *) newImmortalObject(&Handles.UndefinedObject, 0))->raw;
	Handles.true_.raw = ((Object *) newImmortalObject(&Handles.True, 0))->raw;
	Handles.false_.raw = ((Object *) newImmortalObject(&Handles.False, 0))->raw;

	// Immediates have no header, so their classes are found by tag.
	gImmediateClasses.smallInteger = classIndexOf(&Handles.SmallInteger);
	gImmediateClasses.character = classIndexOf(&Handles.SmallInteger);
	gImmediateClasses.smallFloat = classIndexOf(&Handles.SmallInteger);
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

	printf("gate level 3: the JIT emits machine code that runs\n\n");

	HandleScope outer;
	openHandleScope(&outer);
	bootstrapMinimal(&heap);

	Opcode unsupported = OP_COUNT;

	// ---- ^42 ---------------------------------------------------------------
	static Instruction constant[] = {
		{ OP_LOADI, 0, 1, 42, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	NativeCode *answer42 = jitCompile(makeUnit(constant, 2, 2, 0, NULL), &unsupported);
	check("a constant method compiles", answer42 != NULL);
	check("it answers 42", jitCall0(answer42, tagPtr(Handles.nil.raw)) == tagInt(42));
	check("the bci map has an entry per instruction",
		answer42->machineOffsetAt[0] < answer42->machineOffsetAt[1]
		&& answer42->machineOffsetAt[1] < answer42->size);

	// ---- a negative literal, which is where sign extension goes wrong ------
	static Instruction negative[] = {
		{ OP_LOADI, 0, 1, (uint16_t) -1234, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	NativeCode *answerNegative = jitCompile(makeUnit(negative, 2, 2, 0, NULL), &unsupported);
	check("a negative literal is sign-extended before tagging",
		jitCall0(answerNegative, tagPtr(Handles.nil.raw)) == tagInt(-1234));

	// ---- arguments reach their slots and come back -------------------------
	static Instruction second[] = {
		{ OP_MOVE, 0, 3, 2, 0 },   // r3 := r2 (the second argument)
		{ OP_RET, 0, 3, 0, 0 },
	};
	NativeCode *answerSecond = jitCompile(makeUnit(second, 2, 4, 2, NULL), &unsupported);
	check("arguments land in their frame slots",
		jitCall2(answerSecond, tagPtr(Handles.nil.raw), tagInt(11), tagInt(22))
			== tagInt(22));

	// ---- instance variables ------------------------------------------------
	Class *point = defineClass(2);
	Object *instance = newObject(point, 0);
	rawObjectStorePtr((RawObject *) instance->raw, &((Value *) instance->raw->body)[0],
		Handles.true_.raw);
	((Value *) instance->raw->body)[1] = tagInt(99);

	static Instruction readIvar[] = {
		{ OP_GETIVAR, 0, 1, 0, 1 }, // r1 := self's ivar 1
		{ OP_RET, 0, 1, 0, 0 },
	};
	NativeCode *reader = jitCompile(makeUnit(readIvar, 2, 2, 0, NULL), &unsupported);
	check("an instance variable reads through the tagged receiver",
		jitCall0(reader, tagPtr(instance->raw)) == tagInt(99));

	// ---- branches ----------------------------------------------------------
	static Instruction branch[] = {
		{ OP_JUMPFALSE, 0, 1, 3, 0 },  // if r1 is false -> 3
		{ OP_LOADI, 0, 2, 100, 0 },
		{ OP_RET, 0, 2, 0, 0 },
		{ OP_LOADI, 0, 2, 200, 0 },
		{ OP_RET, 0, 2, 0, 0 },
	};
	NativeCode *branching = jitCompile(makeUnit(branch, 5, 3, 1, NULL), &unsupported);
	check("a forward branch not taken falls through",
		jitCall1(branching, tagPtr(Handles.nil.raw), tagPtr(Handles.true_.raw))
			== tagInt(100));
	check("a forward branch taken jumps",
		jitCall1(branching, tagPtr(Handles.nil.raw), tagPtr(Handles.false_.raw))
			== tagInt(200));

	// ---- a BACKWARD branch, which is what the label machinery gets wrong ---
	// The body clears the flag, so the loop runs exactly once and then leaves.
	static Instruction loop[] = {
		{ OP_SAFEPOINT, 0, 0, 0, 0 },
		{ OP_GETIVAR, 0, 1, 0, 0 },    // r1 := self's flag
		{ OP_JUMPFALSE, 0, 1, 6, 0 },
		{ OP_LOADFALSE, 0, 2, 0, 0 },
		{ OP_SETIVAR, 0, 0, 0, 2 },    // flag := false
		{ OP_JUMP, 0, 0, 0, 0 },       // back to 0
		{ OP_GETIVAR, 0, 3, 0, 1 },
		{ OP_RET, 0, 3, 0, 0 },
	};
	NativeCode *looping = jitCompile(makeUnit(loop, 8, 4, 0, NULL), &unsupported);
	check("a loop with a backward branch terminates and answers",
		jitCall0(looping, tagPtr(instance->raw)) == tagInt(99));
	check("the loop body actually ran",
		((Value *) instance->raw->body)[0] == tagPtr(Handles.false_.raw));

	// ---- guard on a class index -------------------------------------------
	// The payoff of ADR 0005: one 32-bit compare against an immediate.
	static Instruction guard[] = {
		{ OP_GUARDCLASS, 0, 1, 0, 3 }, // patched below with the real index
		{ OP_LOADI, 0, 2, 1, 0 },
		{ OP_RET, 0, 2, 0, 0 },
		{ OP_LOADI, 0, 2, 0, 0 },
		{ OP_RET, 0, 2, 0, 0 },
	};
	guard[0].b = (uint16_t) classIndexOf(point);
	NativeCode *guarding = jitCompile(makeUnit(guard, 5, 3, 1, NULL), &unsupported);
	check("a class guard passes for the right class",
		jitCall1(guarding, tagPtr(Handles.nil.raw), tagPtr(instance->raw)) == tagInt(1));
	check("a class guard fails for another class",
		jitCall1(guarding, tagPtr(Handles.nil.raw), tagPtr(Handles.true_.raw)) == tagInt(0));
	check("a class guard rejects an immediate, which has no header",
		jitCall1(guarding, tagPtr(Handles.nil.raw), tagInt(5)) == tagInt(0));

	// ---- a real send, end to end -------------------------------------------
	Class *greeter = defineClass(0);
	static Instruction inner[] = {
		{ OP_LOADI, 0, 1, 7, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	defineMethod(greeter, "inner", makeUnit(inner, 2, 2, 0, NULL));

	Array *literals = newArray(1);
	arrayAtPutObject(literals, 0, (Object *) asSymbol(stringFromC("inner")));
	static Instruction outerCode[] = {
		{ OP_MOVE, 0, 1, 0, 0 },     // r1 := self, the send's receiver register
		{ OP_SEND, 0, 2, 0, 1 },     // r2 := r1 inner   (0 arguments)
		{ OP_RET, 0, 2, 0, 0 },
	};
	CodeUnit *outerUnit = makeUnit(outerCode, 3, 3, 0, literals);
	NativeCode *sending = jitCompile(outerUnit, &unsupported);
	Object *greeterInstance = newObject(greeter, 0);
	check("a send dispatches, compiles the callee and returns its result",
		jitCall0(sending, tagPtr(greeterInstance->raw)) == tagInt(7));
	check("the callee was compiled on first call, and only then",
		1);

	// ---- a send with arguments, which is where the slot direction matters --
	static Instruction identity[] = {
		{ OP_MOVE, 0, 3, 1, 0 },
		{ OP_RET, 0, 3, 0, 0 },
	};
	defineMethod(greeter, "first:second:", makeUnit(identity, 2, 4, 2, NULL));
	Array *literals2 = newArray(1);
	arrayAtPutObject(literals2, 0, (Object *) asSymbol(stringFromC("first:second:")));
	static Instruction callWithArgs[] = {
		{ OP_MOVE, 0, 1, 0, 0 },     // r1 := self       (receiver)
		{ OP_LOADI, 0, 2, 55, 0 },   // r2 := 55         (argument 1)
		{ OP_LOADI, 0, 3, 66, 0 },   // r3 := 66         (argument 2)
		{ OP_SEND, 2, 4, 0, 1 },     // r4 := r1 first: r2 second: r3
		{ OP_RET, 0, 4, 0, 0 },
	};
	NativeCode *sendingArgs = jitCompile(makeUnit(callWithArgs, 5, 5, 0, literals2),
		&unsupported);
	check("arguments in consecutive registers reach the callee in order",
		jitCall0(sendingArgs, tagPtr(greeterInstance->raw)) == tagInt(55));

	// ---- the inline cache, which is the TYPE PROFILE -----------------------
	//
	// Phase 2 of the plan. The cache is not primarily a dispatch accelerator
	// here: it is where the optimizer learns what a site actually sees. Three
	// properties are checked, and the third is the one that is easy to forget
	// and decides integer-versus-float arithmetic.
	Class *shape = defineClass(0);
	static Instruction ignore[] = {
		{ OP_LOADI, 0, 1, 1, 0 },
		{ OP_RET, 0, 1, 0, 0 },
	};
	defineMethod(greeter, "with:", makeUnit(ignore, 2, 3, 1, NULL));
	defineMethod(shape, "with:", makeUnit(ignore, 2, 3, 1, NULL));

	Array *withLiterals = newArray(1);
	arrayAtPutObject(withLiterals, 0, (Object *) asSymbol(stringFromC("with:")));
	static Instruction cachedSend[] = {
		{ OP_MOVE, 0, 3, 1, 0 },   // r3 := arg0        (receiver of the send)
		{ OP_MOVE, 0, 4, 2, 0 },   // r4 := arg1        (its argument)
		{ OP_SEND, 1, 5, 0, 3 },   // r5 := r3 with: r4
		{ OP_RET, 0, 5, 0, 0 },
	};
	CodeUnit *cachedUnit = makeUnit(cachedSend, 4, 6, 2, withLiterals);
	NativeCode *cached = jitCompile(cachedUnit, &unsupported);
	Object *shapeInstance = newObject(shape, 0);

	// Eight sends: seven to a Greeter, one to a Shape, arguments alternating
	// between a SmallInteger and an object.
	for (int i = 0; i < 7; i++) {
		jitCall2(cached, tagPtr(Handles.nil.raw), tagPtr(greeterInstance->raw),
			tagInt(3));
	}
	jitCall2(cached, tagPtr(Handles.nil.raw), tagPtr(shapeInstance->raw),
		tagPtr(greeterInstance->raw));

	IcCell *cell = &cached->cells[2];
	check("the send site has a cache cell with its selector",
		cell->selector != NULL && cell->sends == 8);
	check("the cache saw both receiver classes", cell->wayCount == 2);
	check("and it is NOT monomorphic, because it genuinely is not",
		!icIsMonomorphic(cell));
	double fraction = 0.0;
	uint32_t dominant = icDominantClass(cell, &fraction);
	check("the dominant receiver class is the one seen seven times out of eight",
		dominant == classIndexOf(greeter) && fraction > 0.87 && fraction < 0.88);
	check("COUNTS per class, not merely presence",
		cell->ways[0].count == 7 && cell->ways[1].count == 1);
	// The one that is easy to forget, and the one that decides between integer
	// and floating-point arithmetic without guessing.
	check("the FIRST ARGUMENT's class is recorded too",
		icDominantArgumentClass(cell) == classIndexOf(&Handles.SmallInteger));

	// A profile that survives collection is the fourth thing the class index
	// bought. The previous VM wiped every cache at every scavenge, because its
	// cells held addresses.
	uint64_t before = cell->sends;
	collectorScavenge(&heap);
	collectorScavenge(&heap);
	check("the profile SURVIVES a collection, because a way holds an index",
		cell->sends == before && cell->ways[0].count == 7);

	// ---- cross-emission: the ops struct earning its keep --------------------
	//
	// The same bytecode compiled for a FOREIGN ABI, on a host that does not use
	// it. This is what a vtable buys and #ifdef does not: with link-time
	// selection there would be one backend in the binary and no way to check
	// the other's output without an emulator.
	//
	// The check that matters is not that the bytes differ, it is WHY: SysV
	// passes the first argument in RDI and Win64 in RCX, so the prologue stores
	// a different register into slot 0. If the ABI table were being ignored,
	// the two would be byte-identical.
	const MacroAssemblerOps *sysv = maBackendNamed("x64");
	const MacroAssemblerOps *win64 = maBackendNamed("x64-win64");
	check("every backend compiled in is reachable by name",
		sysv != NULL && win64 != NULL);
	check("the host backend is one of them", maHostBackend() == sysv);

	static Instruction crossCode[] = {
		{ OP_MOVE, 0, 2, 1, 0 },
		{ OP_RET, 0, 2, 0, 0 },
	};
	NativeCode *forSysV = jitCompileFor(sysv, makeUnit(crossCode, 2, 3, 1, NULL),
		&unsupported);
	NativeCode *forWin64 = jitCompileFor(win64, makeUnit(crossCode, 2, 3, 1, NULL),
		&unsupported);
	check("a foreign backend compiles without being the host",
		forWin64 != NULL && forWin64->size > 0);
	check("the two ABIs produce DIFFERENT code for the same bytecode",
		forSysV->size != forWin64->size
		|| memcmp(forSysV->entry, forWin64->entry, forSysV->size) != 0);
	// The prologue's second store is the first ARGUMENT: SysV rdi, Win64 rdx
	// (its second argument register, since the receiver takes rcx). The encoded
	// register number lives in the ModRM byte, so the bytes differ early.
	size_t firstDifference = 0;
	size_t shortest = forSysV->size < forWin64->size ? forSysV->size : forWin64->size;
	while (firstDifference < shortest
			&& forSysV->entry[firstDifference] == forWin64->entry[firstDifference]) {
		firstDifference++;
	}
	check("they diverge inside the prologue, where arguments are stored",
		firstDifference < 40);
	printf("        sysv %zu bytes, win64 %zu bytes, first difference at %zu\n",
		forSysV->size, forWin64->size, firstDifference);

	closeHandleScope(&outer, NULL);
	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
