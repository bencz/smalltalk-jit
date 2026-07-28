#include "tools/Bootstrap.h"
#include "tools/ClassBuilder.h"
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
	// THE FLOAT HIERARCHY MIRRORS packages/Core's, and that is not tidiness.
	// Core declares `Float := Number`, `SmallFloat64 := Float` and
	// `BoxedFloat64 := Float`, and the arithmetic lives on FLOAT. The scaffold
	// used to be installed directly on the two concrete classes, which are more
	// specific, so it won every lookup and Core's Float methods never ran again.
	// Invisible while the primitive SUCCEEDS -- float+float works either way --
	// and only the FAILURE path diverged: `3.0 + (1/2)` reported a failed
	// IntAddPrimitive instead of coercing.
	Handles.Float.raw = fixedClass(object, 0)->raw;
	Handles.SmallFloat64.raw = fixedClass(&Handles.Float, 0)->raw;
	Handles.BoxedFloat64.raw = classCreate(&Handles.Float, NULL, rawDouble)->raw;
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
	withMethods(&Handles.Float);
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
	nameClass(&Handles.Float, "Float");
	nameClass(&Handles.SmallFloat64, "SmallFloat64");
	nameClass(&Handles.BoxedFloat64, "BoxedFloat64");
	nameClass(&Handles.Character, "Character");
	nameClass(&Handles.Closure, "Block");

	// THE SYNTAX TREE CLASSES ARE NAMED TOO, and that is not tidiness.
	//
	// The C parser allocates its nodes as instances of these, and the REFLECTIVE
	// compiler (runtime/primitives/Reflect.c) hands one straight to Smalltalk,
	// which then sends it `body`, `selector`, `expressions`. Those methods live
	// in packages/Core/src/Parser/. A class is reopened BY NAME, so leaving
	// these anonymous meant packages/Core defined a SECOND MethodNode, the
	// parser kept building instances of the first, and the image's node
	// understood nothing.
	//
	// That is the scaffold rule (docs/jit-v2/01-gate.md) in the direction that
	// is easy to miss: the usual failure is a scaffold method SHADOWING the real
	// one, and this is the same defect seen from the other side -- a scaffold
	// CLASS that the real definition never reaches, so the two coexist forever.
	//
	// The slot counts were already derived from Ast.h and they agree with what
	// packages/Core declares, so naming them is all that was missing. The leaf
	// nodes get 2 here and declare none of their own there, inheriting both from
	// LiteralNode.
	nameClass(&Handles.SourceCode, "SourceCode");
	nameClass(&Handles.ClassNode, "ClassNode");
	nameClass(&Handles.MethodNode, "MethodNode");
	nameClass(&Handles.BlockNode, "BlockNode");
	nameClass(&Handles.ExpressionNode, "ExpressionNode");
	nameClass(&Handles.MessageExpressionNode, "MessageExpressionNode");
	nameClass(&Handles.IntegerNode, "IntegerNode");
	nameClass(&Handles.StringNode, "StringNode");
	nameClass(&Handles.SymbolNode, "SymbolNode");
	nameClass(&Handles.CharacterNode, "CharacterNode");
	nameClass(&Handles.ArrayNode, "ArrayNode");
	nameClass(&Handles.VariableNode, "VariableNode");
	nameClass(&Handles.NilNode, "NilNode");
	nameClass(&Handles.TrueNode, "TrueNode");
	nameClass(&Handles.FalseNode, "FalseNode");

	// THE GLOBALS DICTIONARY, UNDER ITS OWN NAME. `Smalltalk` is how image code
	// reaches the system dictionary (`Smalltalk at: #Foo`), and it is also the
	// object identity that makes the Core namespace what it is: Namespace.st
	// states the invariant "(Namespaces at: #Core) bindings == Smalltalk", and
	// core/Namespace.c decides "is this Core?" by comparing that identity rather
	// than by comparing a name a package could spoof.
	//
	// It is registered IN ITSELF, which is not a trick: a dictionary holding an
	// Association to itself is an ordinary cycle and the collector already
	// handles cycles.
	globalAtPut(asSymbol(stringFromC("Smalltalk")),
		objectTagged(smalltalkGlobals()));
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
//
// The message NAMES THE PRIMITIVE, and it is the same message the front end
// emits for a bodyless primitive method in packages/Core (compiler/Compile.c).
// One shape for both kernels: before, the same source meant "fail loudly" here
// and "answer the receiver" there.
static void definePrimitiveMethod(Class *class, const char *selector,
	PrimitiveNumber primitive, uint16_t argumentCount)
{
	HandleScope scope;
	openHandleScope(&scope);

	uint16_t base = (uint16_t) (argumentCount + 1);
	Instruction *code = calloc(4, sizeof(Instruction));
	ASSERT(code != NULL);
	code[0] = (Instruction) { OP_MOVE, 0, base, 0, 0 };   // the receiver
	code[1] = (Instruction) { OP_LOADK, 0,               // which primitive
		(uint16_t) (base + 1), 1, 0 };
	code[2] = (Instruction) { OP_SEND, 1, base, 0, base }; // #primitiveFailed:
	code[3] = (Instruction) { OP_RET, 0, base, 0, 0 };

	Array *literals = newArray(2);
	arrayAtPutObject(literals, 0, (Object *) asSymbol(stringFromC("primitiveFailed:")));
	arrayAtPutObject(literals, 1,
		(Object *) asSymbol(stringFromC(primitiveName(primitive))));
	String *name = asSymbol(stringFromC(selector));

	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	ASSERT(unit != NULL);
	unit->code = code;
	unit->instructionCount = 4;
	unit->registerCount = (uint16_t) (argumentCount + 3);
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
	CompileContext context = { class, smalltalkGlobals(), NULL, NULL };
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

	// SmallInteger gets its own because packages/Core also puts SmallInteger's
	// arithmetic on SmallInteger: same class, so the real definition OVERWRITES
	// the scaffold. The floats get theirs on Float for exactly that reason --
	// see the hierarchy note above.
	defineNumberMethods(&Handles.SmallInteger);
	defineNumberMethods(&Handles.Float);

	// ON OBJECT, and this is scaffolding's ONE placement rule: a scaffold method
	// goes where packages/Core puts the real one, or it SHADOWS it forever.
	//
	// These were on Array, ByteArray and String, one copy each, because that is
	// where an indexed collection's `at:` obviously belongs. But packages/Core
	// declares `at:` ONCE, on Object, and lets the primitive decide by FORMAT
	// (runtime/Primitives.def) -- so a more specific scaffold method won every
	// lookup and packages/Core's version never ran.
	//
	// Invisible while the primitive SUCCEEDS, because both do the same thing.
	// What differed was the failure path: the scaffold's fallback says
	// `primitiveFailed:` and stops, and the real one signals an OutOfRangeError
	// that a handler can catch. So `[#(1 2 3) at: 9] on: Error do: [...]` could
	// not work, and the reason had nothing to do with exceptions.
	definePrimitiveMethod(&Handles.ObjectClass, "at:", PRIM_At, 1);
	definePrimitiveMethod(&Handles.ObjectClass, "at:put:", PRIM_AtPut, 2);
	definePrimitiveMethod(&Handles.ObjectClass, "size", PRIM_Size, 0);

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


// ---------------------------------------------------------------------------
// Loading a package
// ---------------------------------------------------------------------------

static char *readWholeFile(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (length < 0) {
		fclose(file);
		return NULL;
	}
	char *text = malloc((size_t) length + 1);
	if (text == NULL) {
		fclose(file);
		return NULL;
	}
	size_t read = fread(text, 1, (size_t) length, file);
	fclose(file);
	text[read] = '\0';
	if (size != NULL) {
		*size = read;
	}
	return text;
}


// Step over a Smalltalk comment, which is double-quoted. Comments are the whole
// reason this is a state machine and not two calls to strstr: the Core manifest
// OPENS with a comment that explains this reader, and that comment contains the
// word `files:` and a parenthesis. Reading it as code found an empty list and
// said the manifest listed no files, which was true of the comment and false of
// the manifest.
static const char *skipComment(const char *cursor)
{
	if (*cursor != '"') {
		return cursor;
	}
	const char *end = strchr(cursor + 1, '"');
	return end == NULL ? cursor + strlen(cursor) : end + 1;
}


// The file list out of a manifest, with a scanner and not with the DSL: find
// `files:` in CODE, then take every single-quoted string up to the closing
// paren, comments and all.
static size_t manifestFiles(const char *text, char ***files)
{
	const char *cursor = text;
	while (*cursor != '\0') {
		if (*cursor == '"') {
			cursor = skipComment(cursor);
			continue;
		}
		if (strncmp(cursor, "files:", 6) == 0) {
			break;
		}
		cursor++;
	}
	if (*cursor == '\0') {
		return 0;
	}
	while (*cursor != '\0' && *cursor != '(') {
		cursor = *cursor == '"' ? skipComment(cursor) : cursor + 1;
	}
	if (*cursor != '(') {
		return 0;
	}
	size_t capacity = 32;
	size_t count = 0;
	char **names = malloc(capacity * sizeof(char *));
	ASSERT(names != NULL);
	for (const char *end = cursor; *end != '\0' && *end != ')'; end++) {
		if (*end == '"') {
			end = skipComment(end) - 1; // a comment inside the array
			continue;
		}
		if (*end != '\'') {
			continue;
		}
		const char *start = end + 1;
		const char *stop = strchr(start, '\'');
		if (stop == NULL) {
			break;
		}
		if (count == capacity) {
			capacity *= 2;
			names = realloc(names, capacity * sizeof(char *));
			ASSERT(names != NULL);
		}
		size_t length = (size_t) (stop - start);
		names[count] = malloc(length + 1);
		ASSERT(names[count] != NULL);
		memcpy(names[count], start, length);
		names[count][length] = '\0';
		count++;
		end = stop;
	}
	*files = names;
	return count;
}


// One source file: a sequence of class definitions.
static _Bool loadFile(const char *path, BootstrapReport *report,
	OrderedCollection *built)
{
	size_t size = 0;
	char *text = readWholeFile(path, &size);
	if (text == NULL) {
		report->error = "cannot read";
		snprintf(report->errorFile, sizeof(report->errorFile), "%s", path);
		return 0;
	}

	HandleScope scope;
	openHandleScope(&scope);
	Parser parser;
	initParser(&parser, stringFromC(text));

	while (!parserAtEnd(&parser)) {
		HandleScope each;
		openHandleScope(&each);
		ClassNode *node = parseClass(&parser);
		if (node == NULL) {
			report->error = "cannot parse";
			snprintf(report->errorFile, sizeof(report->errorFile), "%s", path);
			printParseError(&parser, (char *) path);
			closeHandleScope(&each, NULL);
			closeHandleScope(&scope, NULL);
			freeParser(&parser);
			free(text);
			return 0;
		}
		ClassBuildError error;
		Class *class = classBuild(node, &error);
		if (error.message != NULL) {
			report->classesFailed++;
			char detail[256] = "";
			if (error.what != NULL) {
				snprintf(detail, sizeof(detail), " '%.*s'",
					(int) rawStringSize(error.what->raw), error.what->raw->contents);
			}
			char method[128] = "";
			if (error.inMethod != NULL) {
				snprintf(method, sizeof(method), " in method '%.*s'",
					(int) rawStringSize(error.inMethod->raw),
					error.inMethod->raw->contents);
			}
			fprintf(stderr, "  %s: %s%s%s\n", path, error.message, detail, method);
			if (report->error == NULL) {
				report->error = error.message;
				snprintf(report->errorFile, sizeof(report->errorFile), "%s", path);
				snprintf(report->errorDetail, sizeof(report->errorDetail), "%s%s",
					detail, method);
			}
			closeHandleScope(&each, NULL);
			continue;
		}
		if (class != NULL) {
			if (built != NULL) {
				ordCollAddObject(built, (Object *) class);
			}
			report->classesBuilt++;
			OrderedCollection *methods = classNodeGetMethods(node);
			report->methodsBuilt += methods == NULL ? 0 : ordCollSize(methods);
		}
		closeHandleScope(&each, NULL);
	}

	closeHandleScope(&scope, NULL);
	freeParser(&parser);
	free(text);
	report->filesRead++;
	return 1;
}


void bootstrapLoadPackage(const char *directory, BootstrapReport *report)
{
	memset(report, 0, sizeof(*report));

	char manifestPath[512];
	snprintf(manifestPath, sizeof(manifestPath), "%s/package.st", directory);
	char *manifest = readWholeFile(manifestPath, NULL);
	if (manifest == NULL) {
		report->error = "cannot read the manifest";
		snprintf(report->errorFile, sizeof(report->errorFile), "%s", manifestPath);
		return;
	}

	char **files = NULL;
	size_t count = manifestFiles(manifest, &files);
	if (count == 0) {
		report->error = "the manifest lists no files";
		snprintf(report->errorFile, sizeof(report->errorFile), "%s", manifestPath);
		free(manifest);
		return;
	}

	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *built = newOrdColl(64);

	for (size_t i = 0; i < count; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
		if (!loadFile(path, report, built)) {
			break;
		}
	}

	// CLASS-SIDE `initialize`, in the order the classes were built.
	//
	// A class that has one is not optional scaffolding: `ExternalStream class
	// initialize` is what makes Transcript a stream on descriptor 1, and until it
	// runs the kernel's own printNl sends nextPutAll: to nil. The order is the
	// manifest's, which is the same order the classes were defined in, because
	// one initializer routinely uses a class defined earlier.
	//
	// ONLY THE CLASS THAT DEFINES ONE RUNS IT, and this is a LOOKUP THAT DOES NOT
	// INHERIT, deliberately. A class-side method is inherited down the metaclass
	// chain like any other, so an ordinary send finds the SUPERCLASS's initialize
	// for every subclass that has none of its own, and the loader then runs it
	// once per subclass with a different `self` each time.
	//
	// Measured, and it is not hypothetical: `ExternalStream class initialize`
	// assigns `Transcript := self descriptor: 1`, and FileStream and Socket
	// inherit it and are built after it, so Transcript ended up a SOCKET on
	// descriptor 1. Everything printed through it then went into the socket
	// write path.
	size_t built_count = ordCollSize(built);
	String *initialize = asSymbol(stringFromC("initialize"));
	for (size_t i = 0; i < built_count; i++) {
		HandleScope each;
		openHandleScope(&each);
		Object *class = ordCollObjectAt(built, i);
		RawClass *metaclass = classOf(objectTagged(class));
		Value methods = metaclass == NULL ? 0 : metaclass->methodDictionary;
		_Bool definesOwn = valueTypeOf(methods, VALUE_POINTER)
			&& valueTypeOf(symbolDictAt(scopeHandle(asObject(methods)), initialize),
				VALUE_POINTER);
		if (definesOwn) {
			_Bool understood = 0;
			jitSendUnary(objectTagged(class), "initialize", &understood);
			report->initializersRun += understood ? 1 : 0;
		}
		closeHandleScope(&each, NULL);
	}
	closeHandleScope(&scope, NULL);
	for (size_t i = 0; i < count; i++) {
		free(files[i]);
	}
	free(files);
	free(manifest);
}
