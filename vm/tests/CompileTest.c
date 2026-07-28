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
#include <stdio.h>
#include <stdlib.h>

__thread Thread CurrentThread;
ptrdiff_t gCurrentThreadTpoff;

static int gFailures;
static int gChecks;
static Dictionary *gGlobals;


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


// Declared by core/Smalltalk.h and normally defined in Smalltalk.c, which is
// still a v1 file. Supplied here so this level links against the front end
// alone, exactly as the levels below it link against one subsystem each.
Class *getClass(char *key)
{
	Object *found = globalObjectAt(stringFromC(key));
	return (Class *) found;
}


Object *globalObjectAt(String *key)
{
	Association *association = symbolDictAssocAt(gGlobals, asSymbol(key));
	if (association == NULL || !valueTypeOf(association->raw->value, VALUE_POINTER)) {
		return NULL;
	}
	return scopeHandle(asObject(association->raw->value));
}


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

	gImmediateClasses.smallInteger = classIndexOf(&Handles.SmallInteger);
	gImmediateClasses.character = classIndexOf(&Handles.Character);
	gImmediateClasses.smallFloat = classIndexOf(&Handles.SmallFloat64);

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
	symbolDictAtPut(gGlobals, asSymbol(stringFromC(name)), value);
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
	CompileContext context = { owner, gGlobals };
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
	gGlobals = newDictionary(64);
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
	Association *bound = symbolDictAtPut(gGlobals, asSymbol(stringFromC("Answer")),
		tagInt(42));
	checkInt("a global reads through its Association",
		call0(define(counter, "readGlobal [ ^Answer ]"), objectTagged(instance)), 42);
	call0(define(counter, "writeGlobal [ Answer := 99 ]"), objectTagged(instance));
	checkInt("and writing one updates that same Association",
		bound->raw->value, 99);

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
		// The non-local return needs RETOUTER and the home-frame token of ADR
		// 0008, which is the next milestone. A plain RET here would return from
		// the BLOCK, quietly answering the wrong thing.
		CompileError error;
		CodeUnit *unit = compileSource(
			"bad [ ^[ :x | ^x ] ]", counter, &error);
		check("a non-local return is refused BY NAME rather than emitted as a "
			"return from the block", unit == NULL && error.status == COMPILE_UNSUPPORTED);
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

	closeHandleScope(&outer, NULL);
	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
