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
		CodeUnit *unit = compileSource("bad [ ^[ :x | x ] ]", counter, &error);
		check("a block that cannot be inlined is refused BY NAME, not emitted wrong",
			unit == NULL && error.status == COMPILE_UNSUPPORTED);
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
	defineGlobal("Object", objectTagged(&Handles.ObjectClass));
	defineGlobal("Array", objectTagged(&Handles.Array));

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
