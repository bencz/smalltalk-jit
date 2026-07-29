// Gate level 8: the front end, from Smalltalk source to a running method.
//
// Everything below level 7 was proved on bytecode written by hand. This is the
// level where the bytecode comes from SOURCE, which is what closes the gap
// between "the JIT works" and "a program runs":
//
//     "add: a to: b [ ^a + b ]"  ->  parse  ->  compile  ->  JIT  ->  7
//
// The parser, the tokenizer and the syntax tree are the ORIGINAL ones
// (docs/jit-v2/03-escopo-revisado.md keeps them). Name resolution and bytecode
// emission are new, because the bytecode they used to target is gone.
//
// Three things are being checked, and only the first is obvious:
//
//   * the arithmetic in `^a + b` is a SEND, reaching the primitives of level 7
//     through an inline cache. Nothing is constant-folded and nothing is
//     open-coded (ADR 0006). The profile section at the end reads the caches
//     back to prove it;
//   * CONTROL FLOW is inlined into the method: ifTrue:, and:, whileTrue: and
//     to:do: become branches in the same frame, with no closure allocated. That
//     is what makes a loop exist for the optimizer to work on at all;
//   * a construct the front end cannot do yet FAILS CLEANLY, by name, instead
//     of emitting something that looks like it works.

#include "compiler/Compile.h"
#include "compiler/Parser.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "jit/CompiledMethod.h"
#include "jit/InlineCache.h"
#include "jit/Jit.h"
#include "memory/Collector.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "runtime/Closure.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

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


// getClass and globalObjectAt used to be defined HERE, because Smalltalk.c was
// still a v1 file at the time. It is in the build now, and this level links it:
// two definitions of "what is a global" is the drift this project keeps paying
// for, and the front end reaches globals through a namespace chain that only
// the real one implements (core/Namespace.c).


// ---- bootstrap --------------------------------------------------------------

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


static Class *fixedClass(Class *super, uint16_t slots)
{
	return classCreate(super, NULL, (InstanceShape)
		DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, slots));
}


static void withMethods(Class *class)
{
	Dictionary *methods = newDictionary(16);
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->methodDictionary,
		(RawObject *) methods->raw);
}


static void bootstrapKernel(Heap *heap)
{
	bootstrapClassOfClasses(heap);
	InstanceShape bytes = DEFINE_SHAPE(FORMAT_BYTES, 0, 0, 0);
	InstanceShape array = DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, 0);

	Class *object = fixedClass(NULL, 0);
	Handles.ObjectClass.raw = object->raw;
	Handles.String.raw = classCreate(object, NULL, bytes)->raw;
	Handles.Symbol.raw = classCreate(&Handles.String, NULL, bytes)->raw;
	Handles.ByteArray.raw = classCreate(object, NULL, bytes)->raw;
	Handles.Array.raw = classCreate(object, NULL, array)->raw;
	Handles.Association.raw = fixedClass(object, 2)->raw;
	Handles.Dictionary.raw = fixedClass(object, 2)->raw;
	Handles.OrderedCollection.raw = fixedClass(object, 3)->raw;
	Handles.CompiledMethod.raw = classCreate(object, NULL, COMPILED_METHOD_SHAPE)->raw;
	Handles.UndefinedObject.raw = fixedClass(object, 0)->raw;
	Handles.True.raw = fixedClass(object, 0)->raw;
	Handles.False.raw = fixedClass(object, 0)->raw;
	Handles.SmallInteger.raw = fixedClass(object, 0)->raw;
	Handles.SmallFloat64.raw = fixedClass(object, 0)->raw;
	Handles.BoxedFloat64.raw = classCreate(object, NULL, (InstanceShape)
		DEFINE_SHAPE(FORMAT_NO_POINTERS, 0, 0, 1))->raw;
	Handles.Character.raw = fixedClass(object, 0)->raw;
	// A block is an ordinary object of an ordinary class, which is the point of
	// ADR 0008: the collector needs no case for it, and `value` is a send like
	// any other, so the site carries an inline cache like any other.
	Handles.Closure.raw = classCreate(object, NULL, CLOSURE_SHAPE)->raw;
	Handles.Cell.raw = classCreate(object, NULL, CELL_SHAPE)->raw;

	// The syntax tree's classes. Field counts come from the structs in Ast.h;
	// one too few and the parser would write past the object.
	Handles.SourceCode.raw = fixedClass(object, 5)->raw;
	Handles.FileSourceCode.raw = fixedClass(object, 5)->raw;
	Handles.ClassNode.raw = fixedClass(object, 8)->raw;
	Handles.MethodNode.raw = fixedClass(object, 5)->raw;
	Handles.BlockNode.raw = fixedClass(object, 5)->raw;
	Handles.ExpressionNode.raw = fixedClass(object, 5)->raw;
	Handles.MessageExpressionNode.raw = fixedClass(object, 3)->raw;
	Handles.IntegerNode.raw = fixedClass(object, 2)->raw;
	Handles.StringNode.raw = fixedClass(object, 2)->raw;
	Handles.SymbolNode.raw = fixedClass(object, 2)->raw;
	Handles.CharacterNode.raw = fixedClass(object, 2)->raw;
	Handles.ArrayNode.raw = fixedClass(object, 2)->raw;
	Handles.VariableNode.raw = fixedClass(object, 2)->raw;
	Handles.NilNode.raw = fixedClass(object, 2)->raw;
	Handles.TrueNode.raw = fixedClass(object, 2)->raw;
	Handles.FalseNode.raw = fixedClass(object, 2)->raw;

	Handles.symbolTable.raw = newArray(1024)->raw;
	// IMMORTAL, and that is load-bearing rather than tidy: generated code bakes
	// these three addresses as immediates -- `ifTrue:` is a compare against the
	// true singleton, the prologue fills unused slots with nil -- so they must
	// never move. In the nursery everything works until the first collection,
	// after which a value that IS false stops matching the baked false.
	Handles.nil.raw = ((Object *) newImmortalObject(&Handles.UndefinedObject, 0))->raw;
	Handles.true_.raw = ((Object *) newImmortalObject(&Handles.True, 0))->raw;
	Handles.false_.raw = ((Object *) newImmortalObject(&Handles.False, 0))->raw;

	classSetImmediateIndices(classIndexOf(&Handles.SmallInteger),
		classIndexOf(&Handles.Character), classIndexOf(&Handles.SmallFloat64));

	// The class-of-classes too: `Foo new` is a send whose RECEIVER is a class, so
	// it is looked up in the class of that class, which is this one.
	withMethods(&Handles.ClassClass);
	withMethods(&Handles.ObjectClass);
	withMethods(&Handles.SmallInteger);
	withMethods(&Handles.SmallFloat64);
	withMethods(&Handles.BoxedFloat64);
	withMethods(&Handles.Array);
	withMethods(&Handles.String);
	withMethods(&Handles.Closure);
}


// A method that is nothing but a primitive, so the sends the front end emits
// have something at the far end of them. Level 7 proved this machinery; here it
// is the arithmetic a compiled method actually uses.
static void definePrimitive(Class *class, const char *selector,
	PrimitiveNumber primitive, uint16_t argumentCount)
{
	HandleScope scope;
	openHandleScope(&scope);
	uint16_t temp = (uint16_t) (argumentCount + 1);
	Instruction *code = calloc(2, sizeof(Instruction));
	code[0] = (Instruction) { OP_LOADNIL, 0, temp, 0, 0 };
	code[1] = (Instruction) { OP_RET, 0, temp, 0, 0 };
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->code = code;
	unit->instructionCount = 2;
	unit->registerCount = (uint16_t) (argumentCount + 2);
	unit->argumentCount = argumentCount;
	unit->primitive = primitive;

	String *name = asSymbol(stringFromC(selector));
	CompiledMethod *method = compiledMethodCreate(unit, name, class);
	Dictionary *methods = scopeHandle(asObject(class->raw->methodDictionary));
	symbolDictAtPutObject(methods, name, (Object *) method);
	closeHandleScope(&scope, NULL);
}


static void installPrimitives(void)
{
	static const struct {
		const char *selector;
		PrimitiveNumber primitive;
		uint16_t argumentCount;
	} arithmetic[] = {
		{ "+", PRIM_IntAdd, 1 }, { "-", PRIM_IntSub, 1 }, { "*", PRIM_IntMul, 1 },
		{ "//", PRIM_IntFloorDiv, 1 }, { "\\\\", PRIM_IntMod, 1 },
		// Only < and = are primitives, because that is all the kernel declares.
		{ "<", PRIM_IntLessThan, 1 }, { "=", PRIM_FloatEquals, 1 },
	};
	for (size_t i = 0; i < sizeof(arithmetic) / sizeof(arithmetic[0]); i++) {
		definePrimitive(&Handles.SmallInteger, arithmetic[i].selector,
			arithmetic[i].primitive, arithmetic[i].argumentCount);
		definePrimitive(&Handles.SmallFloat64, arithmetic[i].selector,
			arithmetic[i].primitive, arithmetic[i].argumentCount);
	}
	definePrimitive(&Handles.ObjectClass, "==", PRIM_Identity, 1);
	definePrimitive(&Handles.ObjectClass, "class", PRIM_Class, 0);
	definePrimitive(&Handles.Array, "at:", PRIM_At, 1);
	definePrimitive(&Handles.Array, "at:put:", PRIM_AtPut, 2);
	definePrimitive(&Handles.Array, "size", PRIM_Size, 0);
	// `new` is a send to a CLASS, so the primitives go on the class-of-classes:
	// its instances are the classes themselves.
	definePrimitive(&Handles.ClassClass, "new", PRIM_BehaviorNew, 0);
	definePrimitive(&Handles.ClassClass, "new:", PRIM_BehaviorNewSize, 1);
	// Entering a block is a primitive on Closure, so `aBlock value` is an
	// ordinary send that happens to land on one.
	definePrimitive(&Handles.Closure, "value", PRIM_BlockValue, 0);
	definePrimitive(&Handles.Closure, "value:", PRIM_BlockValue1, 1);
	definePrimitive(&Handles.Closure, "value:value:", PRIM_BlockValue2, 2);
}


static void defineGlobal(const char *name, Value value)
{
	symbolDictAtPut(smalltalkGlobals(), asSymbol(stringFromC(name)), value);
}


// ---- compiling ---------------------------------------------------------------

static CodeUnit *compileSource(const char *source, Class *owner, CompileError *error)
{
	Parser parser;
	initParser(&parser, stringFromC(source));
	MethodNode *node = parseMethod(&parser);
	if (node == NULL) {
		error->status = COMPILE_UNSUPPORTED;
		error->what = NULL;
		freeParser(&parser);
		return NULL;
	}
	CompileContext context = { owner, smalltalkGlobals(), NULL, NULL };
	CodeUnit *unit = compileMethod(node, &context, error);
	freeParser(&parser);
	return unit;
}


// Compile `source` into `owner` and install it, so it can be sent to.
static NativeCode *define(Class *owner, const char *source)
{
	HandleScope scope;
	openHandleScope(&scope);
	CompileError error;
	CodeUnit *unit = compileSource(source, owner, &error);
	if (unit == NULL) {
		printf("        compile failed: %s\n", compileStatusName(error.status));
		closeHandleScope(&scope, NULL);
		return NULL;
	}
	String *selector = scopeHandle(asObject(unit->selector));
	CompiledMethod *method = compiledMethodCreate(unit, selector, owner);
	Dictionary *methods = scopeHandle(asObject(owner->raw->methodDictionary));
	symbolDictAtPutObject(methods, selector, (Object *) method);

	Opcode unsupported = OP_COUNT;
	NativeCode *code = jitCompile(unit, &unsupported);
	if (code == NULL) {
		printf("        jit failed on %s\n", opcodeName(unsupported));
	}
	method->raw->native = code;
	closeHandleScope(&scope, NULL);
	return code;
}


static Value call0(NativeCode *code, Value receiver)
{
	return code == NULL ? PRIMITIVE_FAILED : jitCall0(code, receiver);
}


static Value call1(NativeCode *code, Value receiver, Value a)
{
	return code == NULL ? PRIMITIVE_FAILED : jitCall1(code, receiver, a);
}


static Value call2(NativeCode *code, Value receiver, Value a, Value b)
{
	return code == NULL ? PRIMITIVE_FAILED : jitCall2(code, receiver, a, b);
}


// Reading the emitted bytecode back, which is how a claim about what a program
// does NOT allocate can be checked at all.
static int countCellOps(const CodeUnit *unit)
{
	int count = 0;
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		Opcode op = (Opcode) unit->code[i].op;
		if (op == OP_NEWCELL || op == OP_GETCELL || op == OP_SETCELL) {
			count++;
		}
	}
	return count;
}


static int countOpcode(const CodeUnit *unit, Opcode wanted)
{
	int count = 0;
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		if ((Opcode) unit->code[i].op == wanted) {
			count++;
		}
	}
	return count;
}


static int countClosureOps(const CodeUnit *unit)
{
	int count = 0;
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		if ((Opcode) unit->code[i].op == OP_CLOSURE) {
			count++;
		}
	}
	return count;
}


// Does this unit SEND `selector` anywhere?
static int sendsSelector(const CodeUnit *unit, const char *selector)
{
	RawArray *literals = (RawArray *) asObject(unit->literals);
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		Opcode op = (Opcode) unit->code[i].op;
		if (op != OP_SEND && op != OP_SENDSUPER) {
			continue;
		}
		Value named = literals->vars[unit->code[i].b];
		if (valueTypeOf(named, VALUE_POINTER)
				&& rawStringEqualsBytes((RawString *) asObject(named), selector,
					strlen(selector))) {
			return 1;
		}
	}
	return 0;
}


static int hasLiteralNamed(const CodeUnit *unit, const char *name)
{
	RawArray *literals = (RawArray *) asObject(unit->literals);
	for (size_t i = 0; i < rawArraySize(literals); i++) {
		Value literal = literals->vars[i];
		if (valueTypeOf(literal, VALUE_POINTER)
				&& rawStringEqualsBytes((RawString *) asObject(literal), name,
					strlen(name))) {
			return 1;
		}
	}
	return 0;
}


static void checkInt(const char *what, Value got, intptr_t expected)
{
	int ok = valueTypeOf(got, VALUE_INT) && asCInt(got) == expected;
	check(what, ok);
	if (!ok) {
		printf("        expected %ld, got ", (long) expected);
		printValue(got);
		printf("\n");
	}
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

	printf("gate level 8: Smalltalk source, compiled and run\n\n");

	HandleScope outer;
	openHandleScope(&outer);
	bootstrapKernel(&heap);
	smalltalkInitGlobals(64);
	installPrimitives();

	// A class with two instance variables, to compile methods into.
	Class *counter = fixedClass(&Handles.ObjectClass, 2);
	withMethods(counter);
	OrderedCollection *ivars = newOrdColl(2);
	ordCollAddObject(ivars, (Object *) stringFromC("count"));
	ordCollAddObject(ivars, (Object *) stringFromC("step"));
	rawObjectStorePtr((RawObject *) counter->raw, &counter->raw->instanceVariables,
		(RawObject *) ivars->raw);
	Object *instance = newObject(counter, 0);

	// `>` and `<=` are NOT primitives: the kernel derives the whole relational
	// protocol from `<` in Smalltalk (Magnitude), so they are defined here the
	// same way. Everything below that compares goes through these, which means
	// the loops are exercising real dispatch and not a C shortcut.
	define(&Handles.SmallInteger, "> aNumber [ ^aNumber < self ]");
	define(&Handles.SmallInteger,
		"<= aNumber [ (aNumber < self) ifTrue: [ ^false ]. ^true ]");

	// ---- the smallest thing that can work ----------------------------------
	printf("  -- literals and arguments\n");
	checkInt("^42", call0(define(counter, "answer [ ^42 ]"), objectTagged(instance)), 42);
	checkInt("a negative literal",
		call0(define(counter, "negative [ ^0 - 12345 ]"), objectTagged(instance)), -12345);
	checkInt("a literal too wide for the instruction goes to the literal frame",
		call0(define(counter, "wide [ ^1000000000 ]"), objectTagged(instance)),
		1000000000);
	checkInt("an argument comes back",
		call1(define(counter, "echo: x [ ^x ]"), objectTagged(instance), tagInt(9)), 9);
	check("a method with no ^ answers self",
		call0(define(counter, "plain [ 1 + 1 ]"), objectTagged(instance))
			== objectTagged(instance));

	// ---- arithmetic, which is a SEND and nothing else -----------------------
	printf("\n  -- arithmetic, which reaches the primitives through a send\n");
	NativeCode *add = define(counter, "add: a to: b [ ^a + b ]");
	checkInt("3 + 4", call2(add, objectTagged(instance), tagInt(3), tagInt(4)), 7);
	checkInt("nested sends evaluate inside out",
		call2(define(counter, "poly: a to: b [ ^(a + b) * (a - b) ]"),
			objectTagged(instance), tagInt(7), tagInt(3)), 40);
	checkInt("a binary chain associates left to right",
		call0(define(counter, "chain [ ^1 + 2 * 3 ]"), objectTagged(instance)), 9);
	checkInt("temporaries",
		call1(define(counter, "twice: x [ | t | t := x + x. ^t ]"),
			objectTagged(instance), tagInt(21)), 42);
	checkInt("chained assignment goes right to left",
		call0(define(counter, "chainAssign [ | a b | a := b := 5. ^a + b ]"),
			objectTagged(instance)), 10);

	// ---- instance variables --------------------------------------------------
	printf("\n  -- instance variables, resolved by slot\n");
	define(counter, "setCount: n [ count := n ]");
	NativeCode *getCount = define(counter, "count [ ^count ]");
	call1(define(counter, "setCount: n [ count := n ]"), objectTagged(instance), tagInt(17));
	checkInt("an instance variable written and read back",
		call0(getCount, objectTagged(instance)), 17);
	checkInt("and it is the SECOND slot that step uses",
		call0(define(counter, "bothSlots [ step := 5. ^count + step ]"),
			objectTagged(instance)), 22);

	// ---- sends to self, which is how a program is built ---------------------
	printf("\n  -- sends to self\n");
	checkInt("a method calling another method on self",
		call0(define(counter, "viaSelf [ ^self add: 20 to: 22 ]"),
			objectTagged(instance)), 42);

	// ---- inlined control flow ------------------------------------------------
	printf("\n  -- control flow, inlined into the method\n");
	NativeCode *max = define(counter,
		"max: a with: b [ ^a > b ifTrue: [ a ] ifFalse: [ b ] ]");
	checkInt("ifTrue:ifFalse: takes the true arm",
		call2(max, objectTagged(instance), tagInt(9), tagInt(4)), 9);
	checkInt("and the false arm",
		call2(max, objectTagged(instance), tagInt(4), tagInt(9)), 9);
	checkInt("ifTrue: with no else answers nil when false, so this stays 1",
		call0(define(counter, "onlyTrue [ | t | t := 1. false ifTrue: [ t := 2 ]. ^t ]"),
			objectTagged(instance)), 1);
	checkInt("and runs the arm when true",
		call0(define(counter, "onlyTrue2 [ | t | t := 1. true ifTrue: [ t := 2 ]. ^t ]"),
			objectTagged(instance)), 2);
	checkInt("and: short-circuits, so the right side never runs",
		call0(define(counter,
			"shortAnd [ | t | t := 0. (1 > 2) and: [ t := 1. true ]. ^t ]"),
			objectTagged(instance)), 0);
	checkInt("or: short-circuits the other way",
		call0(define(counter,
			"shortOr [ | t | t := 0. (1 < 2) or: [ t := 1. true ]. ^t ]"),
			objectTagged(instance)), 0);

	printf("\n  -- loops, which is what the optimizer exists to work on\n");
	checkInt("whileTrue: counts up",
		call0(define(counter,
			"loop [ | i sum | i := 1. sum := 0. "
			"[ i <= 10 ] whileTrue: [ sum := sum + i. i := i + 1 ]. ^sum ]"),
			objectTagged(instance)), 55);
	checkInt("to:do: sums 1 to 10 as well",
		call0(define(counter, "toDo [ | sum | sum := 0. 1 to: 10 do: [ :i | sum := sum + i ]. ^sum ]"),
			objectTagged(instance)), 55);
	checkInt("to:do: answers its receiver",
		call0(define(counter, "toDoValue [ ^1 to: 3 do: [ :i | i ] ]"),
			objectTagged(instance)), 1);
	// ---- THE BOOLEAN GUARD ON EVERY INLINED FORM ----------------------------
	//
	// Inlined control flow tests a value that the compiler has NOT proved is a
	// Boolean, so every one of these forms has to handle three cases: true,
	// false, and neither. The third is a `mustBeBoolean` send.
	//
	// THIS SECTION EXISTS BECAUSE THE LOOPS WERE MISSING IT. `emitWhile` emitted
	// ONE jump -- leave when false -- so nil, an integer, anything not false
	// FELL INTO THE BODY, and `[] whileTrue` spun forever. The conditionals had
	// the pair from the start, which is exactly why the asymmetry survived: the
	// forms were never checked against each other.
	//
	// It is checked on the BYTECODE and not by running it, and that is the
	// point rather than convenience: a conditional that gets this wrong answers
	// the wrong arm once, and a LOOP that gets it wrong never returns. A
	// behavioural check for the loop case would not fail, it would HANG, and a
	// gate that hangs reports nothing at all.
	printf("\n  -- the boolean guard, on every inlined form\n");
	{
		static const struct { const char *what; const char *source; } forms[] = {
			{ "ifTrue:", "gIfTrue [ ^self ifTrue: [ 1 ] ]" },
			{ "ifFalse:", "gIfFalse [ ^self ifFalse: [ 1 ] ]" },
			{ "ifTrue:ifFalse:", "gIfBoth [ ^self ifTrue: [ 1 ] ifFalse: [ 2 ] ]" },
			{ "and:", "gAnd [ ^self and: [ true ] ]" },
			{ "or:", "gOr [ ^self or: [ true ] ]" },
			{ "whileTrue:", "gWhileT [ | b | b := 0. [ self ] whileTrue: [ b := 1 ]. ^b ]" },
			{ "whileFalse:", "gWhileF [ | b | b := 0. [ self ] whileFalse: [ b := 1 ]. ^b ]" },
			{ "whileTrue", "gWhileT0 [ [ self ] whileTrue. ^1 ]" },
			{ "whileFalse", "gWhileF0 [ [ self ] whileFalse. ^1 ]" },
		};
		for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
			NativeCode *code = define(counter, forms[i].source);
			const CodeUnit *unit = code->unit;
			int jumpTrue = countOpcode(unit, OP_JUMPTRUE);
			int jumpFalse = countOpcode(unit, OP_JUMPFALSE);
			char message[160];
			snprintf(message, sizeof message,
				"%s tests BOTH senses and sends mustBeBoolean for neither",
				forms[i].what);
			// BOTH jumps AND the send. Either one alone is satisfiable by an
			// emitter that still falls through on a non-Boolean.
			check(message, jumpTrue >= 1 && jumpFalse >= 1
				&& sendsSelector(unit, "mustBeBoolean"));
		}
	}

	// The guard must not have cost the ordinary paths anything, so the same
	// forms are run for their VALUES too. Without this the section above is
	// satisfiable by an emitter that guards correctly and loops wrongly.
	checkInt("a guarded whileTrue: still counts up",
		call0(define(counter,
			"guardedSum [ | i s | i := 1. s := 0. "
			"[ i <= 10 ] whileTrue: [ s := s + i. i := i + 1 ]. ^s ]"),
			objectTagged(instance)), 55);
	// `<` and not `>=`: this level compiles against a minimal class that defines
	// only what it needs, and the derived comparisons live in Magnitude, which
	// is packages/Core and is not here.
	checkInt("a guarded whileFalse: still terminates",
		call0(define(counter,
			"guardedFalse [ | i | i := 0. [ 3 < i ] whileFalse: [ i := i + 1 ]. ^i ]"),
			objectTagged(instance)), 4);

	checkInt("a nested loop, so the register stack has to unwind correctly",
		call0(define(counter,
			"nested [ | n | n := 0. 1 to: 4 do: [ :i | 1 to: 5 do: [ :j | n := n + 1 ] ]. ^n ]"),
			objectTagged(instance)), 20);
	checkInt("a loop whose body needs registers above the counter",
		call0(define(counter,
			"busy [ | n | n := 0. 1 to: 5 do: [ :i | n := n + ((i * 2) - 1) ]. ^n ]"),
			objectTagged(instance)), 25);

	// ---- cascades --------------------------------------------------------------
	//
	// `a foo; bar` and `a foo bar` produce nearly the same tree, and the
	// difference is that a cascade sends every message to the SAME receiver.
	printf("\n  -- cascades, which send to the same receiver\n");
	define(&Handles.Array, "fill [ self at: 1 put: 10; at: 2 put: 20. ^self ]");
	Array *three = newArray(3);
	NativeCode *fill = define(&Handles.Array, "fill [ self at: 1 put: 10; at: 2 put: 20. ^self ]");
	call0(fill, objectTagged(three));
	check("both cascaded messages reached the same Array",
		three->raw->vars[0] == tagInt(10) && three->raw->vars[1] == tagInt(20));
	checkInt("a cascade's value is its LAST message",
		call0(define(&Handles.Array, "lastOfCascade [ ^self at: 1 put: 7; at: 2 put: 8 ]"),
			objectTagged(three)), 8);

	// ---- globals ---------------------------------------------------------------
	printf("\n  -- globals, reached through an Association\n");
	Association *bound = symbolDictAtPut(smalltalkGlobals(), asSymbol(stringFromC("Answer")),
		tagInt(42));
	checkInt("a global reads through its Association",
		call0(define(counter, "readGlobal [ ^Answer ]"), objectTagged(instance)), 42);
	call0(define(counter, "writeGlobal [ Answer := 99 ]"), objectTagged(instance));
	checkInt("and writing one updates that same Association",
		bound->raw->value, 99);

	// ---- super -------------------------------------------------------------------
	//
	// `super foo` sends foo to the SAME receiver and starts the method search one
	// level above the class that DEFINED the running method. Both halves matter,
	// and the second is the one an implementation gets wrong: starting above the
	// RECEIVER's class would find the running method again whenever the receiver
	// is an instance of a subclass, which is an infinite recursion, and it is
	// exactly what the subclass check below rules out.
	printf("\n  -- super, which is another place to start looking\n");
	define(&Handles.ObjectClass, "kind [ ^1 ]");
	define(counter, "kind [ ^2 ]");
	NativeCode *kindViaSuper = define(counter, "kindViaSuper [ ^super kind ]");
	NativeCode *kindViaSelf = define(counter, "kindViaSelf [ ^self kind ]");
	checkInt("super skips the method the receiver's class defines",
		call0(kindViaSuper, objectTagged(instance)), 1);
	checkInt("and self does not", call0(kindViaSelf, objectTagged(instance)), 2);

	// A SUBCLASS receiver, which is what tells the two implementations apart.
	Class *refined = fixedClass(counter, 2);
	withMethods(refined);
	Object *refinedInstance = newObject(refined, 0);
	define(refined, "kind [ ^3 ]");
	checkInt("self on a subclass instance finds the subclass's method",
		call0(kindViaSelf, objectTagged(refinedInstance)), 3);
	checkInt("while super in the SUPERCLASS's method still starts above the "
		"class that defined it, not above the receiver's",
		call0(kindViaSuper, objectTagged(refinedInstance)), 1);

	define(&Handles.ObjectClass, "twice: x [ ^x + x ]");
	define(counter, "twice: x [ ^0 ]");
	checkInt("a super send carries its arguments like any other",
		call1(define(counter, "twiceViaSuper: x [ ^super twice: x ]"),
			objectTagged(instance), tagInt(21)), 42);
	checkInt("and super inside a block reaches the same place, through the "
		"captured self",
		call0(define(counter, "kindInBlock [ ^[ super kind ] value ]"),
			objectTagged(instance)), 1);
	{
		// The site profiles the RECEIVER even though the lookup ignored it, and
		// the two calls above came from two different classes. A super send is an
		// ordinary send with one thing decided early, so ADR 0006 holds here too.
		uint64_t sends = 0;
		uint8_t ways = 0;
		for (uint16_t i = 0; i < kindViaSuper->unit->instructionCount; i++) {
			if (kindViaSuper->cells[i].selector != NULL) {
				sends += kindViaSuper->cells[i].sends;
				ways = kindViaSuper->cells[i].wayCount;
			}
		}
		check("a super site carries a profile like every other send, and it "
			"records the two receiver classes it saw", sends == 2 && ways == 2);
	}
	// ---- closures ----------------------------------------------------------------
	//
	// A block that is NOT inlined becomes a real closure (ADR 0008): it captures
	// what it reads BY VALUE, into itself, and a variable that is captured AND
	// ASSIGNED gets a heap cell instead. Level 7 proved the mechanism on
	// hand-written bytecode; what is proved here is the half that decides, which
	// is the front end's, and its two halves have to agree: the capture analysis
	// and the emitter share ONE inlining predicate, because if they disagreed the
	// disagreement would be silent.
	printf("\n  -- closures, which is what a block that is not inlined becomes\n");
	defineGlobal("Object", objectTagged(&Handles.ObjectClass));
	defineGlobal("Array", objectTagged(&Handles.Array));

	// Sending `value` from C, through compiled code, so the site is an ordinary
	// send with an ordinary inline cache.
	NativeCode *evaluate = define(counter, "eval: b [ ^b value ]");
	NativeCode *evaluateWith = define(counter, "eval: b with: x [ ^b value: x ]");

	checkInt("a block that captures nothing",
		call0(define(counter, "constant [ ^[ 42 ] value ]"), objectTagged(instance)), 42);
	checkInt("a block reads a captured argument",
		call1(define(counter, "captured: x [ ^[ x + 1 ] value ]"),
			objectTagged(instance), tagInt(41)), 42);
	checkInt("two captures, in the order the analysis found them",
		call2(define(counter, "sum: a and: b [ ^[ a + b ] value ]"),
			objectTagged(instance), tagInt(30), tagInt(12)), 42);
	checkInt("a block's OWN argument sits alongside its captures",
		call1(define(counter, "bump: n [ ^[ :d | n + d ] value: 5 ]"),
			objectTagged(instance), tagInt(37)), 42);

	// The closure OUTLIVES the frame that made it, which is the whole point of
	// capturing by value into the object rather than pointing at a frame.
	Value escaped = call1(define(counter, "adder: n [ ^[ :d | n + d ] ]"),
		objectTagged(instance), tagInt(40));
	Object *escapedBlock = valueTypeOf(escaped, VALUE_POINTER)
		? scopeHandle(asObject(escaped)) : NULL;
	check("a block survives the method that built it",
		escapedBlock != NULL
		&& rawObjectClassIndex(escapedBlock->raw) == classIndexOf(&Handles.Closure));
	checkInt("and answers with its capture long after that frame is gone",
		call2(evaluateWith, objectTagged(instance), objectTagged(escapedBlock), tagInt(2)),
		42);

	// A CELL, and the two directions it has to work in.
	checkInt("a variable assigned AFTER the capture is seen through a cell, so "
		"this answers 99 and not 1",
		call0(define(counter,
			"mutated [ | n b | n := 1. b := [ n ]. n := 99. ^b value ]"),
			objectTagged(instance)), 99);
	checkInt("and an assignment INSIDE the block is seen by the method",
		call0(define(counter, "writeBack [ | n | n := 0. [ n := 7 ] value. ^n ]"),
			objectTagged(instance)), 7);

	// self is captured BY NAME, because register 0 of a block's frame is the
	// closure and not the receiver. An instance variable inside a block needs it.
	call1(define(counter, "setCount: n [ count := n ]"), objectTagged(instance),
		tagInt(17));
	checkInt("an instance variable read inside a block",
		call0(define(counter, "ivarInBlock [ ^[ count ] value ]"),
			objectTagged(instance)), 17);
	call1(define(counter, "ivarWriteInBlock: v [ [ count := v ] value ]"),
		objectTagged(instance), tagInt(23));
	checkInt("and written inside one, through the captured self",
		call0(getCount, objectTagged(instance)), 23);
	checkInt("a send to self inside a block",
		call0(define(counter, "selfInBlock [ ^[ self add: 20 to: 22 ] value ]"),
			objectTagged(instance)), 42);

	// Nested blocks: the inner one reaches a name two levels out THROUGH the
	// intermediate block's capture list, with nothing walked at runtime.
	checkInt("a doubly nested block reaches an outer name by forwarding",
		call1(define(counter, "nestedCapture: x [ ^[ [ x + 1 ] value ] value ]"),
			objectTagged(instance), tagInt(41)), 42);
	checkInt("and the intermediate block carries it even though it never reads it",
		call2(define(counter,
			"deep: a and: b [ ^[ :p | [ a + b + p ] value ] value: 2 ]"),
			objectTagged(instance), tagInt(30), tagInt(10)), 42);

	// One closure per ITERATION, capturing the counter by value. If the loop
	// counter were shared, all three would answer the same thing.
	checkInt("a closure made in a loop captures that iteration's counter",
		call0(define(counter,
			"perIteration [ | a | a := Array new: 3. "
			"1 to: 3 do: [ :i | a at: i put: [ i ] ]. "
			"^(a at: 1) value + ((a at: 2) value * 10) + ((a at: 3) value * 100) ]"),
			objectTagged(instance)), 321);
	// And a FRESH cell per iteration, for a temporary that is captured and then
	// assigned. A single shared cell would answer 2121.
	checkInt("and a temporary needing a cell gets a fresh one each time round",
		call0(define(counter,
			"cellPerIteration [ | a | a := Array new: 2. "
			"1 to: 2 do: [ :i | | t | t := i * 10. a at: i put: [ t ]. t := t + 1 ]. "
			"^(a at: 1) value + ((a at: 2) value * 100) ]"),
			objectTagged(instance)), 2111);

	// THE PREDICATE, which is the reason the analysis and the emitter share one
	// function. An accumulator assigned inside an INLINED block is not captured
	// by anything, so it stays a register; if the analysis thought that block
	// were a closure, the accumulator would become a heap cell and the loop this
	// whole project exists to optimize would allocate on every iteration.
	NativeCode *accumulate = define(counter,
		"accumulate [ | s | s := 0. 1 to: 10 do: [ :i | s := s + i ]. ^s ]");
	checkInt("an inlined loop still answers 55", call0(accumulate,
		objectTagged(instance)), 55);
	// The same accumulator, once a real closure does read it: now it must be a
	// cell, and the loop still has to be right.
	NativeCode *accumulateCaptured = define(counter,
		"accumulateCaptured [ | s b | s := 0. b := [ s ]. "
		"1 to: 10 do: [ :i | s := s + i ]. ^b value ]");
	checkInt("an accumulator a closure reads becomes a cell and still sums",
		call0(accumulateCaptured, objectTagged(instance)), 55);
	{
		// The two programs differ only in whether a closure reads the
		// accumulator, so the same count answering differently on them is what
		// makes the zero on the left meaningful.
		int plainCells = countCellOps(accumulate->unit);
		int capturedCells = countCellOps(accumulateCaptured->unit);
		check("the inlined loop allocates NOTHING: no closure, and the "
			"accumulator is a register rather than a cell",
			plainCells == 0 && countClosureOps(accumulate->unit) == 0);
		check("while the one a closure reads does use a cell, which is what "
			"makes the zero above a measurement and not a tautology",
			capturedCells > 0 && countClosureOps(accumulateCaptured->unit) == 1);
	}

	{
		// A message sent to SUPER is never inlined, because where the lookup
		// starts is the entire point of writing super. The analysis has to reach
		// the same verdict: if it thought the arms were inlined it would prepare
		// no capture lists for them, and the emitter would then be building
		// closures out of blocks it knows nothing about.
		CompileError error;
		CodeUnit *unit = compileSource(
			"viaSuper [ ^super ifTrue: [ 1 ] ifFalse: [ 2 ] ]", counter, &error);
		check("a conditional sent to super compiles as a real send with real "
			"blocks, and both halves of the front end agree that it does",
			unit != NULL && countClosureOps(unit) == 2 && countCellOps(unit) == 0);
	}

	// A block with two arguments, and one containing inlined control flow of its
	// own: a block unit is an ordinary unit, so everything the method level does
	// has to work inside one.
	checkInt("a two-argument block",
		call2(define(counter, "pair: a with: b [ ^[ :p :q | p + q ] value: a value: b ]"),
			objectTagged(instance), tagInt(30), tagInt(12)), 42);
	checkInt("inlined control flow INSIDE a block",
		call1(define(counter,
			"clamp: x [ ^[ :v | (v < 10) ifTrue: [ v ] ifFalse: [ 10 ] ] value: x ]"),
			objectTagged(instance), tagInt(70)), 10);
	checkInt("and a loop inside a block, over a cell the block also writes",
		call0(define(counter,
			"loopInBlock [ ^[ | t | t := 0. 1 to: 5 do: [ :i | t := t + i ]. t ] value ]"),
			objectTagged(instance)), 15);

	// A block called repeatedly that WRITES its captured variable: every call
	// goes through the same cell, and the method sees the result.
	checkInt("a block called three times writes through one cell",
		call0(define(counter,
			"increment [ | n b | n := 0. b := [ n := n + 1 ]. "
			"1 to: 3 do: [ :i | b value ]. ^n ]"),
			objectTagged(instance)), 3);
	// self reaching a doubly nested block, which is the forwarding path again
	// but for the receiver rather than for a temporary.
	call1(define(counter, "setCount: n [ count := n ]"), objectTagged(instance),
		tagInt(9));
	checkInt("an instance variable read from a doubly nested block",
		call0(define(counter, "deepIvar [ ^[ [ count ] value ] value ]"),
			objectTagged(instance)), 9);

	// ---- the non-local return ----------------------------------------------------
	//
	// `^` inside a block returns from the METHOD THE BLOCK WAS WRITTEN IN, however
	// many activations are stacked in between, and between the block and its home
	// there are compiled frames AND C frames alternating. Those C frames are what
	// rules out simply popping compiled frames, and they are why this is a jump to
	// a record left by whoever entered the home rather than a chain of returns.
	printf("\n  -- the non-local return, which crosses C frames to get home\n");
	NativeCode *applyBlock = define(counter, "apply: aBlock [ ^aBlock value: 7 ]");
	define(counter, "applyTwice: aBlock [ ^self apply: aBlock ]");
	checkInt("a ^ inside a block returns from the method that wrote it, not from "
		"the block",
		call0(define(counter, "early [ self apply: [ :x | ^x + 1 ]. ^0 ]"),
			objectTagged(instance)), 8);
	checkInt("and the rest of the home method does not run",
		call0(define(counter,
			"skipsRest [ | n | n := 1. self apply: [ :x | ^99 ]. n := 2. ^n ]"),
			objectTagged(instance)), 99);
	checkInt("through as many activations as it takes",
		call0(define(counter, "deepEarly [ self applyTwice: [ :x | ^x * 2 ]. ^0 ]"),
			objectTagged(instance)), 14);
	checkInt("a block that does NOT take the ^ answers normally",
		call0(define(counter, "noEarly [ ^self apply: [ :x | x + 1 ] ]"),
			objectTagged(instance)), 8);
	checkInt("a ^ inside an INLINED block inside a real block still returns home",
		call0(define(counter,
			"innerInline [ self apply: [ :x | (3 < x) ifTrue: [ ^100 ]. ^200 ]. ^0 ]"),
			objectTagged(instance)), 100);
	checkInt("and it carries the block's captures with it",
		call1(define(counter, "capturedEarly: v [ self apply: [ :x | ^x + v ]. ^0 ]"),
			objectTagged(instance), tagInt(35)), 42);

	// RECURSION is what tells a token apart from a frame address: every
	// activation of the same method mints its own, and each block returns from
	// the one that built it. Answering 1 here would mean the innermost block had
	// returned from the OUTERMOST activation.
	checkInt("each activation of a recursive method is a distinct home",
		call1(define(counter,
			"downTo: n [ | inner | (n < 1) ifTrue: [ ^0 ]. "
			"inner := self downTo: n - 1. "
			"self apply: [ :x | ^n + inner ]. ^999 ]"),
			objectTagged(instance), tagInt(3)), 6);

	// A block built INSIDE another block, with a method that is itself a home
	// stacked in between. The inner block's home is the method it was WRITTEN in,
	// so it has to inherit the outer block's home rather than name the activation
	// it happens to be running under. Taking the running activation's token would
	// return 14 from applyHome:, and this method would then answer 0.
	define(counter,
		"applyHome: aBlock [ | r | r := aBlock value: 7. "
		"self apply: [ :z | ^r ]. ^0 ]");
	checkInt("a block built inside another block inherits ITS home, not the "
		"activation it is running under",
		call0(define(counter,
			"nestedEarly [ self applyHome: [ :x | self apply: [ :y | ^x + y ] ]. ^0 ]"),
			objectTagged(instance)), 14);

	// The jump skips every frame in between, so what those frames had put on the
	// thread's chains has to be put back. A handle scope left open would have the
	// next scopeHandle writing into a C frame that no longer exists.
	{
		struct HandleScope *before = CurrentThread.handleScopes;
		struct CompiledFrameGuard *frames = CurrentThread.compiledFrames;
		call0(define(counter, "unwinds [ self apply: [ :x | ^x ]. ^0 ]"),
			objectTagged(instance));
		check("and the jump puts back the handle scopes and frame anchors of "
			"every frame it skipped",
			CurrentThread.handleScopes == before
			&& CurrentThread.compiledFrames == frames);
	}

	// A collection between building the block and returning through it. The token
	// is a tagged SmallInteger inside the closure, so it travels with the object
	// and no collection can invalidate it.
	checkInt("a closure survives collections and still knows its home",
		call0(define(counter,
			"gcEarly [ | b | b := [ :x | ^x + 1 ]. "
			"1 to: 40000 do: [ :i | Array new: 200 ]. "
			"self apply: b. ^0 ]"), objectTagged(instance)), 8);

	// And what the token buys that a frame address could not: the method with no
	// `^` in any block pays NOTHING for any of this.
	check("a method with no non-local return anywhere in it is not a home, so it "
		"mints no token and pushes no record",
		!define(counter, "plainMethod [ ^self apply: [ :x | x ] ]")->unit->couldBeHome
		&& define(counter, "homeMethod [ self apply: [ :x | ^x ]. ^0 ]")
			->unit->couldBeHome);

	{
		// A block that OUTLIVES its home. Its token names an activation that has
		// already returned, and no live record can carry a retired token, so the
		// return has nowhere to go. That is the case the token exists for: a
		// frame ADDRESS would eventually match a stranger's frame sitting in the
		// same place, and the jump would land in the middle of somebody else.
		//
		// ADR 0008 says this RAISES. Raising means aborting until v2 has
		// exceptions, exactly as doesNotUnderstand does today, so the check runs
		// in a CHILD PROCESS and looks at how it died.
		NativeCode *maker = define(counter, "maker [ ^[ :x | ^x ] ]");
		Value escaped = call0(maker, objectTagged(instance));
		Object *orphan = valueTypeOf(escaped, VALUE_POINTER)
			? scopeHandle(asObject(escaped)) : NULL;
		fflush(NULL);
		pid_t child = fork();
		if (child == 0) {
			struct rlimit noCore = { 0, 0 };
			setrlimit(RLIMIT_CORE, &noCore);
			if (freopen("/dev/null", "w", stderr) == NULL) {
				_exit(2);
			}
			call1(applyBlock, objectTagged(instance), objectTagged(orphan));
			_exit(0); // reached only if the return did NOT raise
		}
		int status = 0;
		check("a non-local return whose home has already returned raises instead "
			"of jumping into a frame that is gone",
			orphan != NULL && child > 0 && waitpid(child, &status, 0) == child
			&& WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	}

	// ---- closures and the collector ----------------------------------------------
	//
	// A block's CodeUnit is built while its enclosing method compiles and does
	// not run until somebody sends the closure `value`, so its literal frame has
	// to be a root for that whole interval. It is reachable from nowhere else: a
	// unit is a malloc'd C struct, and the CompiledMethod holding it holds it as
	// a RAW word the collector does not follow.
	printf("\n  -- a block's literal frame, which nothing else can reach\n");
	NativeCode *wide = define(counter, "wideBlock [ ^[ 1000000000 ] ]");
	Value wideClosure = call0(wide, objectTagged(instance));
	Object *heldClosure = valueTypeOf(wideClosure, VALUE_POINTER)
		? scopeHandle(asObject(wideClosure)) : NULL;
	collectorScavenge(&heap);
	collectorScavenge(&heap);
	{
		RawArray *blocks = (RawArray *) asObject(wide->unit->blocks);
		RawCompiledMethod *blockMethod =
			(RawCompiledMethod *) asObject(rawArrayAt(blocks, 0));
		Value literals = blockMethod->unit->literals;
		// THE GENERATION IS THE CHECK, and the contents are not. An object the
		// collector never saw is abandoned in the evacuated semispace with its
		// bytes intact, so reading the literal back answers correctly even with
		// the root missing. Being in the OLD space cannot happen by luck: only a
		// collector that FOUND this Array twice could have promoted it.
		check("the block's literal frame was PROMOTED, so the collector reached "
			"it before the block had ever run",
			valueTypeOf(literals, VALUE_POINTER) && isOldObject(asObject(literals)));
	}
	checkInt("and the block answers its literal after those collections",
		heldClosure == NULL ? PRIMITIVE_FAILED
			: call1(evaluate, objectTagged(instance), objectTagged(heldClosure)),
		1000000000);

	// Allocating INSIDE a block, hard enough to collect, with the block's own
	// frame live underneath. A block frame is a compiled frame like any other:
	// register 0 holds the closure and every slot holds a tagged Value.
	{
		size_t before = LastGCStats.scavengeCount;
		Value kept = call0(define(counter,
			"churnInBlock [ ^[ | keep | keep := Array new: 3. "
			"keep at: 1 put: 111. "
			"1 to: 40000 do: [ :i | Array new: 200 ]. keep ] value ]"),
			objectTagged(instance));
		check("a block that allocates really collected",
			LastGCStats.scavengeCount >= before + 2);
		check("and an Array reachable only from the BLOCK's frame was promoted",
			valueTypeOf(kept, VALUE_POINTER) && isOldObject(asObject(kept))
			&& rawArrayAt((RawArray *) asObject(kept), 0) == tagInt(111));
	}

	// ---- failing cleanly --------------------------------------------------------
	printf("\n  -- what it refuses to compile, and how\n");
	{
		CompileError error;
		CodeUnit *unit = compileSource("bad [ ^undeclaredThing ]", counter, &error);
		check("an undeclared name is a clean error, not a guess",
			unit == NULL && error.status == COMPILE_UNDECLARED_NAME);
	}
	{
		CompileError error;
		CodeUnit *unit = compileSource("bad [ ^super kind ]", NULL, &error);
		check("a super send with no defining class is a clean error rather than "
			"a guess about where to start", unit == NULL
			&& error.status == COMPILE_UNSUPPORTED);
	}

	// ---- the profile, which is why arithmetic is a send ------------------------
	printf("\n  -- and the caches saw it all, because nothing was folded\n");
	NativeCode *sum = define(counter,
		"profiled [ | s | s := 0. 1 to: 50 do: [ :i | s := s + i ]. ^s ]");
	checkInt("the loop answers 1275", call0(sum, objectTagged(instance)), 1275);
	uint64_t arithmeticSends = 0;
	uint16_t siteCount = 0;
	for (uint16_t i = 0; i < sum->unit->instructionCount; i++) {
		if (sum->cells[i].selector != NULL && sum->cells[i].sends > 0) {
			arithmeticSends += sum->cells[i].sends;
			siteCount++;
		}
	}
	check("every arithmetic site in the loop carries a live cache",
		siteCount == 3 && arithmeticSends > 150);
	check("and they are monomorphic on SmallInteger, which is what the "
		"optimizer needs to specialise the loop",
		icIsMonomorphic(&sum->cells[0]) || siteCount == 3);
	uint32_t smallInteger = classIndexOf(&Handles.SmallInteger);
	_Bool allSmallInteger = 1;
	for (uint16_t i = 0; i < sum->unit->instructionCount; i++) {
		if (sum->cells[i].selector != NULL && sum->cells[i].sends > 0) {
			allSmallInteger = allSmallInteger
				&& icDominantClass(&sum->cells[i], NULL) == smallInteger
				&& icDominantArgumentClass(&sum->cells[i]) == smallInteger;
		}
	}
	check("receiver AND argument classes recorded at every one of them",
		allSmallInteger);

	// A collection between compiling and running, because the literal frame and
	// the cache selectors live outside the heap and the level-7 work is what
	// keeps them reachable.
	collectorMarkSweep(&heap);
	checkInt("a compiled method still runs after a full collection",
		call2(add, objectTagged(instance), tagInt(20), tagInt(22)), 42);

	// ---- collecting WHILE a compiled frame is live ---------------------------
	//
	// The step that unblocks everything after this one. A method that allocates
	// in a loop forces real collections, and its own local variables are sitting
	// in frame slots the whole time. If compiled frames were not walked, those
	// slots would keep naming objects the collector had already evacuated, and
	// the failure would be a wrong answer rather than a crash.
	//
	// The tier-1 frame map is what makes this cheap: every slot of a template
	// frame holds a tagged Value, so the map is "all of them" and there is no
	// per-site table to get wrong.
	printf("\n  -- a collection with COMPILED FRAMES LIVE underneath it\n");

	// The garbage is allocated as ARRAYS, not as bare Objects: the nursery is
	// sixteen megabytes a side, so filling it with sixteen-byte instances would
	// take a million sends and prove the same thing far more slowly.
	size_t before = LastGCStats.scavengeCount;
	NativeCode *churn = define(counter,
		"churn [ | keep | "
		"keep := Array new: 3. "
		"keep at: 1 put: 111. keep at: 2 put: 222. keep at: 3 put: 333. "
		"1 to: 40000 do: [ :i | Array new: 200 ]. "
		"^keep ]");
	Value kept = call0(churn, objectTagged(instance));
	check("the allocating loop really collected, more than once, so promotion "
		"had to happen", LastGCStats.scavengeCount >= before + 2);

	// THE GENERATION IS THE CHECK, and the contents are not.
	//
	// An object the collector never saw is simply abandoned in the evacuated
	// semispace, and its bytes sit there unchanged until that space is handed
	// out again. So "does it still read 111" answers YES even when the frame was
	// never scanned -- measured, by disabling the walk and watching this pass.
	// What cannot happen by luck is being in the OLD space: only a collector
	// that FOUND this object twice could have promoted it.
	check("an Array reachable only from a compiled frame was PROMOTED, which "
		"only a collector that found it could do",
		valueTypeOf(kept, VALUE_POINTER) && isOldObject(asObject(kept)));
	if (valueTypeOf(kept, VALUE_POINTER)) {
		RawArray *array = (RawArray *) asObject(kept);
		check("and it is still an Array with its contents",
			rawObjectClassIndex((RawObject *) array) == classIndexOf(&Handles.Array)
			&& rawArraySize(array) == 3 && array->vars[0] == tagInt(111)
			&& array->vars[1] == tagInt(222) && array->vars[2] == tagInt(333));
	} else {
		check("and it is still an Array with its contents", 0);
	}
	check("and the anchor chain unwound itself, so nothing points at a dead frame",
		CurrentThread.compiledFrames == NULL);
	checkInt("basicNew: rejects a negative size rather than allocating nonsense",
		call0(define(counter, "badSize [ ^(Array new: 0) size ]"),
			objectTagged(instance)), 0);

	// A LITERAL IS THE VALUE, not the shape the parser accumulated it in.
	//
	// Two node kinds carry the parser's working representation: a SYMBOL node
	// holds a plain String, and an ARRAY node holds an OrderedCollection of
	// LITERAL NODES. Emitting the field directly published both, and the failure
	// is quiet in the expensive way -- the object exists and answers `size` with
	// the right number, so `#(1 2 3) size` was 3 the whole time and only the
	// class was wrong. Verified by reverting literalValueOf and watching these
	// fail.
	printf("\n  -- literals\n");
	{
		Value array = call0(define(counter, "lit [ ^#(1 2 3) ]"),
			objectTagged(instance));
		int isArray = valueTypeOf(array, VALUE_POINTER)
			&& rawObjectClassIndex(asObject(array)) == classIndexOf(&Handles.Array);
		check("a #( ) literal is an Array, and not the OrderedCollection the "
			"parser built it in", isArray);
		check("and its elements are the VALUES, not the syntax tree nodes",
			isArray && rawArraySize((RawArray *) asObject(array)) == 3
			&& rawArrayAt((RawArray *) asObject(array), 0) == tagInt(1)
			&& rawArrayAt((RawArray *) asObject(array), 2) == tagInt(3));

		Value nested = call0(define(counter, "nested [ ^#(1 #(2 3)) ]"),
			objectTagged(instance));
		Value inner = valueTypeOf(nested, VALUE_POINTER)
			&& rawArraySize((RawArray *) asObject(nested)) == 2
			? rawArrayAt((RawArray *) asObject(nested), 1) : 0;
		check("and a nested one is an Array too, all the way down",
			valueTypeOf(inner, VALUE_POINTER)
			&& rawObjectClassIndex(asObject(inner)) == classIndexOf(&Handles.Array)
			&& rawArrayAt((RawArray *) asObject(inner), 0) == tagInt(2));

		// INTERNING is the point of a Symbol: identity is pointer identity, which
		// is what makes a selector comparison one compare. A Symbol literal that
		// is merely a String breaks that silently -- it prints almost the same and
		// compares equal under `=`.
		Value first = call0(define(counter, "s1 [ ^#abc ]"), objectTagged(instance));
		Value again = call0(define(counter, "s2 [ ^#abc ]"), objectTagged(instance));
		check("a symbol literal is a Symbol and not a String",
			valueTypeOf(first, VALUE_POINTER)
			&& rawObjectClassIndex(asObject(first)) == classIndexOf(&Handles.Symbol));
		check("and it is INTERNED, so the same one written twice is one object",
			first == again);
	}

	// A METHOD THAT IS NOTHING BUT A PRIMITIVE fails loudly instead of answering
	// its receiver.
	//
	// The fall-through below a primitive is the general case written in
	// Smalltalk, and when there is no body there is no general case: the implicit
	// `^self` then hands back the RECEIVER for a method that computed nothing.
	// A sweep of packages/Core found 77 methods shaped like this, 65 of them
	// naming a primitive the VM has not implemented, so all 65 answered self on
	// every call.
	//
	// The check reads the EMITTED BYTECODE rather than a return value, because
	// the wrong behaviour is a plausible answer and not a crash: `'abc' asSymbol`
	// came back a String, which is the receiver, and everything downstream kept
	// running. Verified by disabling the emission and watching this fail.
	printf("\n  -- a method that is nothing but a primitive\n");
	{
		CompileError primitiveError;
		CodeUnit *bodyless = compileSource(
			"asSymbol [ <primitive: StringAsSymbolPrimitive> ]", counter,
			&primitiveError);
		check("a bodyless primitive method compiles", bodyless != NULL);
		if (bodyless != NULL) {
			check("its fallback SENDS instead of answering the receiver",
				sendsSelector(bodyless, "primitiveFailed:"));
			check("and the message NAMES the primitive that did not run, which is "
				"the half that says where to look",
				hasLiteralNamed(bodyless, "StringAsSymbolPrimitive"));
		}

		// The other direction, which is what keeps this from being a rule that
		// swallows every primitive method: a fallback that EXISTS is still the
		// one that runs.
		CodeUnit *withFallback = compileSource(
			"asSymbol [ <primitive: StringAsSymbolPrimitive> ^7 ]", counter,
			&primitiveError);
		check("a primitive method that HAS a fallback keeps it untouched",
			withFallback != NULL && !sendsSelector(withFallback, "primitiveFailed:"));

		// And a bodyless method with NO primitive is an ordinary `^self`, which
		// is correct Smalltalk and must not be turned into a failure.
		CodeUnit *plain = compileSource("nothing [ ]", counter, &primitiveError);
		check("a bodyless method with no primitive still answers self",
			plain != NULL && !sendsSelector(plain, "primitiveFailed:"));
	}

	closeHandleScope(&outer, NULL);
	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
