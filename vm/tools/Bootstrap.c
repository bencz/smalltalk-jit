#include "tools/Bootstrap.h"
#include "compiler/Compile.h"
#include "compiler/Parser.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "memory/Heap.h"
#include "runtime/Closure.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------

// The class of classes, which is the one object that cannot be made the ordinary
// way: creating a class needs a class to stamp on it, and this is the first one.
// It describes ITSELF, so its own class index is its own.
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


static Class *withMethods(Class *class)
{
	Dictionary *methods = newDictionary(16);
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->methodDictionary,
		(RawObject *) methods->raw);
	return class;
}


// A name for a class, both as its own `name` field and as a global, so source
// code can say `Array new: 3` and the compiler finds it.
static void nameClass(Class *class, const char *name)
{
	String *symbol = asSymbol(stringFromC(name));
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->name,
		(RawObject *) symbol->raw);
	globalAtPut(symbol, objectTagged(class));
}


static void bootstrapClasses(Heap *heap)
{
	bootstrapClassOfClasses(heap);
	InstanceShape bytes = DEFINE_SHAPE(FORMAT_BYTES, 0, 0, 0);
	InstanceShape indexed = DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, 0);
	InstanceShape rawDouble = DEFINE_SHAPE(FORMAT_NO_POINTERS, 0, 0, 1);

	Class *object = fixedClass(NULL, 0);
	Handles.ObjectClass.raw = object->raw;
	Handles.String.raw = classCreate(object, NULL, bytes)->raw;
	Handles.Symbol.raw = classCreate(&Handles.String, NULL, bytes)->raw;
	Handles.ByteArray.raw = classCreate(object, NULL, bytes)->raw;
	Handles.Array.raw = classCreate(object, NULL, indexed)->raw;
	Handles.Association.raw = fixedClass(object, 2)->raw;
	Handles.Dictionary.raw = fixedClass(object, 2)->raw;
	Handles.OrderedCollection.raw = fixedClass(object, 3)->raw;
	Handles.CompiledMethod.raw = classCreate(object, NULL, COMPILED_METHOD_SHAPE)->raw;
	Handles.UndefinedObject.raw = fixedClass(object, 0)->raw;
	Handles.True.raw = fixedClass(object, 0)->raw;
	Handles.False.raw = fixedClass(object, 0)->raw;
	Handles.SmallInteger.raw = fixedClass(object, 0)->raw;
	Handles.SmallFloat64.raw = fixedClass(object, 0)->raw;
	Handles.BoxedFloat64.raw = classCreate(object, NULL, rawDouble)->raw;
	Handles.Character.raw = fixedClass(object, 0)->raw;
	Handles.Closure.raw = classCreate(object, NULL, CLOSURE_SHAPE)->raw;
	Handles.Cell.raw = classCreate(object, NULL, CELL_SHAPE)->raw;

	// The syntax tree is made of ORDINARY HEAP OBJECTS, so the parser cannot run
	// until its classes exist. The field counts come from the structs in Ast.h;
	// one too few and the parser writes past the object.
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

	Handles.symbolTable.raw = newArray(SYMBOL_TABLE_SIZE)->raw;

	// IMMORTAL, and that is load-bearing rather than tidy: generated code BAKES
	// these three addresses as immediates, so they must never move. In the
	// nursery everything works until the first collection, after which a value
	// that IS false stops matching the baked false. jitCompileFor asserts it on
	// every compilation.
	Handles.nil.raw = ((Object *) newImmortalObject(&Handles.UndefinedObject, 0))->raw;
	Handles.true_.raw = ((Object *) newImmortalObject(&Handles.True, 0))->raw;
	Handles.false_.raw = ((Object *) newImmortalObject(&Handles.False, 0))->raw;

	// Immediates have no header, so their class is found by TAG rather than read
	// out of the object.
	gImmediateClasses.smallInteger = classIndexOf(&Handles.SmallInteger);
	gImmediateClasses.character = classIndexOf(&Handles.Character);
	gImmediateClasses.smallFloat = classIndexOf(&Handles.SmallFloat64);

	// The class-of-classes gets a method dictionary too: `Foo new` is a send
	// whose RECEIVER is a class, so it is looked up in the class of that class.
	withMethods(&Handles.ClassClass);
	withMethods(&Handles.ObjectClass);
	withMethods(&Handles.SmallInteger);
	withMethods(&Handles.SmallFloat64);
	withMethods(&Handles.BoxedFloat64);
	withMethods(&Handles.Character);
	withMethods(&Handles.String);
	withMethods(&Handles.Symbol);
	withMethods(&Handles.ByteArray);
	withMethods(&Handles.Array);
	withMethods(&Handles.UndefinedObject);
	withMethods(&Handles.True);
	withMethods(&Handles.False);
	withMethods(&Handles.Closure);
}


static void nameClasses(void)
{
	nameClass(&Handles.ObjectClass, "Object");
	nameClass(&Handles.ClassClass, "Class");
	nameClass(&Handles.String, "String");
	nameClass(&Handles.Symbol, "Symbol");
	nameClass(&Handles.ByteArray, "ByteArray");
	nameClass(&Handles.Array, "Array");
	nameClass(&Handles.Association, "Association");
	nameClass(&Handles.Dictionary, "Dictionary");
	nameClass(&Handles.OrderedCollection, "OrderedCollection");
	nameClass(&Handles.CompiledMethod, "CompiledMethod");
	nameClass(&Handles.UndefinedObject, "UndefinedObject");
	nameClass(&Handles.True, "True");
	nameClass(&Handles.False, "False");
	nameClass(&Handles.SmallInteger, "SmallInteger");
	nameClass(&Handles.SmallFloat64, "SmallFloat64");
	nameClass(&Handles.BoxedFloat64, "BoxedFloat64");
	nameClass(&Handles.Character, "Character");
	nameClass(&Handles.Closure, "Block");
}


// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

// A method that is nothing but a primitive.
//
// Its fallback SENDS a selector nobody implements, so a primitive that fails in
// the built-in kernel says so and stops. Answering nil instead would turn every
// failure into a wrong answer somewhere else: an overflowed sum has no
// LargeInteger to fall back to here, and an out-of-range at: has no exception to
// signal, because neither of those exists until packages/Core loads.
static void definePrimitiveMethod(Class *class, const char *selector,
	PrimitiveNumber primitive, uint16_t argumentCount)
{
	HandleScope scope;
	openHandleScope(&scope);

	uint16_t base = (uint16_t) (argumentCount + 1);
	Instruction *code = calloc(3, sizeof(Instruction));
	ASSERT(code != NULL);
	code[0] = (Instruction) { OP_MOVE, 0, base, 0, 0 };        // the receiver
	code[1] = (Instruction) { OP_SEND, 0, base, 0, base };     // #primitiveFailed
	code[2] = (Instruction) { OP_RET, 0, base, 0, 0 };

	Array *literals = newArray(1);
	arrayAtPutObject(literals, 0, (Object *) asSymbol(stringFromC("primitiveFailed")));
	String *name = asSymbol(stringFromC(selector));

	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	ASSERT(unit != NULL);
	unit->code = code;
	unit->instructionCount = 3;
	unit->registerCount = (uint16_t) (argumentCount + 2);
	unit->argumentCount = argumentCount;
	unit->primitive = primitive;
	unit->literals = objectTagged(literals);
	unit->selector = objectTagged(name);
	unit->ownerClass = objectTagged(class);
	jitRegisterUnit(unit);

	CompiledMethod *method = compiledMethodCreate(unit, name, class);
	Dictionary *methods = scopeHandle(asObject(class->raw->methodDictionary));
	symbolDictAtPutObject(methods, name, (Object *) method);
	closeHandleScope(&scope, NULL);
}


// A method compiled from SOURCE, which is how the built-in kernel gets the few
// things that are shorter to write in Smalltalk than to hand-assemble. It also
// means the front end runs before any program does, so a bootstrap that got past
// here has already exercised parser, compiler and JIT.
static void defineSourceMethod(Class *class, const char *source)
{
	HandleScope scope;
	openHandleScope(&scope);

	Parser parser;
	initParser(&parser, stringFromC(source));
	MethodNode *node = parseMethod(&parser);
	if (node == NULL) {
		fprintf(stderr, "bootstrap: cannot parse '%s'\n", source);
		abort();
	}
	CompileContext context = { class, smalltalkGlobals() };
	CompileError error;
	CodeUnit *unit = compileMethod(node, &context, &error);
	freeParser(&parser);
	if (unit == NULL) {
		fprintf(stderr, "bootstrap: cannot compile '%s': %s\n", source,
			compileStatusName(error.status));
		abort();
	}
	String *selector = scopeHandle(asObject(unit->selector));
	CompiledMethod *method = compiledMethodCreate(unit, selector, class);
	Dictionary *methods = scopeHandle(asObject(class->raw->methodDictionary));
	symbolDictAtPutObject(methods, selector, (Object *) method);
	closeHandleScope(&scope, NULL);
}


// The arithmetic and comparison primitives, on every class whose instances the
// VM can hold in a register.
//
// Int and Float share ONE implementation for each operation, which is not a
// shortcut: it is the mixed-arithmetic fast path, and it is why `1 + 2.5` does
// not have to become a coercion send here.
static void defineNumberMethods(Class *class)
{
	static const struct {
		const char *selector;
		PrimitiveNumber primitive;
		uint16_t argumentCount;
	} arithmetic[] = {
		{ "+", PRIM_IntAdd, 1 }, { "-", PRIM_IntSub, 1 }, { "*", PRIM_IntMul, 1 },
		{ "/", PRIM_FloatDiv, 1 }, { "//", PRIM_IntFloorDiv, 1 },
		{ "\\\\", PRIM_IntMod, 1 },
		// Only < and = are primitives, because that is all the kernel declares;
		// the rest of the relational protocol is derived below, in Smalltalk,
		// exactly as Magnitude derives it.
		{ "<", PRIM_IntLessThan, 1 }, { "=", PRIM_FloatEquals, 1 },
	};
	for (size_t i = 0; i < sizeof(arithmetic) / sizeof(arithmetic[0]); i++) {
		definePrimitiveMethod(class, arithmetic[i].selector,
			arithmetic[i].primitive, arithmetic[i].argumentCount);
	}
	defineSourceMethod(class, "> aNumber [ ^aNumber < self ]");
	defineSourceMethod(class, "<= aNumber [ ^(aNumber < self) not ]");
	defineSourceMethod(class, ">= aNumber [ ^(self < aNumber) not ]");
	defineSourceMethod(class, "~= aNumber [ ^(self = aNumber) not ]");
}


static void bootstrapMethods(void)
{
	definePrimitiveMethod(&Handles.ObjectClass, "==", PRIM_Identity, 1);
	definePrimitiveMethod(&Handles.ObjectClass, "class", PRIM_Class, 0);
	definePrimitiveMethod(&Handles.ObjectClass, "hash", PRIM_Hash, 0);
	definePrimitiveMethod(&Handles.ObjectClass, "printNl", PRIM_PrintValue, 0);
	definePrimitiveMethod(&Handles.ObjectClass, "displayNl", PRIM_PrintValue, 0);

	// `new` and `new:` are sends whose RECEIVER is a class, so they go on the
	// class of classes: its instances are the classes themselves.
	definePrimitiveMethod(&Handles.ClassClass, "new", PRIM_BehaviorNew, 0);
	definePrimitiveMethod(&Handles.ClassClass, "new:", PRIM_BehaviorNewSize, 1);

	defineNumberMethods(&Handles.SmallInteger);
	defineNumberMethods(&Handles.SmallFloat64);
	defineNumberMethods(&Handles.BoxedFloat64);

	definePrimitiveMethod(&Handles.Array, "at:", PRIM_At, 1);
	definePrimitiveMethod(&Handles.Array, "at:put:", PRIM_AtPut, 2);
	definePrimitiveMethod(&Handles.Array, "size", PRIM_Size, 0);
	definePrimitiveMethod(&Handles.ByteArray, "at:", PRIM_At, 1);
	definePrimitiveMethod(&Handles.ByteArray, "at:put:", PRIM_AtPut, 2);
	definePrimitiveMethod(&Handles.ByteArray, "size", PRIM_Size, 0);
	definePrimitiveMethod(&Handles.String, "at:", PRIM_At, 1);
	definePrimitiveMethod(&Handles.String, "at:put:", PRIM_AtPut, 2);
	definePrimitiveMethod(&Handles.String, "size", PRIM_Size, 0);

	definePrimitiveMethod(&Handles.Closure, "value", PRIM_BlockValue, 0);
	definePrimitiveMethod(&Handles.Closure, "value:", PRIM_BlockValue1, 1);
	definePrimitiveMethod(&Handles.Closure, "value:value:", PRIM_BlockValue2, 2);

	// `not` is what the derived comparisons above are written in terms of, and
	// on a singleton it is a constant.
	defineSourceMethod(&Handles.True, "not [ ^false ]");
	defineSourceMethod(&Handles.False, "not [ ^true ]");
	defineSourceMethod(&Handles.True, "& aBoolean [ ^aBoolean ]");
	defineSourceMethod(&Handles.False, "& aBoolean [ ^false ]");
	defineSourceMethod(&Handles.True, "| aBoolean [ ^true ]");
	defineSourceMethod(&Handles.False, "| aBoolean [ ^aBoolean ]");
	defineSourceMethod(&Handles.UndefinedObject, "isNil [ ^true ]");
	defineSourceMethod(&Handles.ObjectClass, "isNil [ ^false ]");
	defineSourceMethod(&Handles.ObjectClass, "notNil [ ^self isNil not ]");
	defineSourceMethod(&Handles.ObjectClass, "yourself [ ^self ]");
}


void bootstrapBuiltinKernel(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	bootstrapClasses(CurrentThread.heap);
	smalltalkInitGlobals(64);
	nameClasses();
	bootstrapMethods();
	closeHandleScope(&scope, NULL);
}
