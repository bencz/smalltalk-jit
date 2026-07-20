#include "tools/Bootstrap.h"
#include "core/Thread.h"
#include "core/Object.h"
#include "core/Smalltalk.h"
#include "runtime/Dictionary.h"
#include "compiler/Parser.h"
#include "core/Class.h"
#include "compiler/Compiler.h"
#include "runtime/Primitives.h"
#include "memory/Heap.h"
#include "core/Thread.h"
#include "core/Entry.h"
#include "core/Handle.h"
#include "runtime/Iterator.h"
#include "core/Assert.h"
#include "core/Namespace.h"
#include "compiler/Scope.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void initSmalltalkStubs(void);
static Class *newStubClass(Class *metaClass, InstanceShape shape, size_t instanceSize);
static Class *newStubMetaClass(InstanceShape shape, size_t instanceSize);
static Object *newStubObject(size_t size);
static _Bool parseKernelFiles(char *coreDir);


// Builds the initial image from nothing. The C side cannot be replaced by the
// package system because the loader cannot load itself: PackageLoader and
// Compiler evaluate: are kernel Smalltalk, and before the kernel is compiled
// there is no Smalltalk to run them. What stays in C is exactly the
// pre-Smalltalk minimum: the stub metaobjects the parser needs
// (initSmalltalkStubs), the primitive table (registerPrimitives), the parse
// loop over the core files (parseKernelFiles), the metaclass initialize sweep
// and the layout ASSERTs. The FILE LIST is no longer C: the Core package
// manifest (<coreDir>/package.st, normally packages/Core/package.st) is the
// single source of truth for the kernel file set and its order.
_Bool bootstrap(char *coreDir)
{
	initSmalltalkStubs();
	registerPrimitives();
	return parseKernelFiles(coreDir);
}


static void initSmalltalkStubs(void)
{
	HandleScope scope;
	openHandleScope(&scope);

	Handles.nil = newStubObject(sizeof(struct { OBJECT_HEADER; }));
	Class *metaClass = newStubMetaClass(FixedShape, 10);

	Handles.MetaClass = newStubClass(metaClass, FixedShape, 6);
	Handles.UndefinedObject = newStubClass(metaClass, FixedShape, 0);
	Handles.True = newStubClass(metaClass, FixedShape, 0);
	Handles.False = newStubClass(metaClass, FixedShape, 0);
	Handles.SmallInteger = newStubClass(metaClass, FixedShape, 0);
	Handles.Symbol = newStubClass(metaClass, StringShape, 0);
	Handles.Character = newStubClass(metaClass, FixedShape, 0);
	Handles.Float = newStubClass(metaClass, FloatShape, 0);
	Handles.SmallFloat64 = newStubClass(metaClass, FixedShape, 0);
	Handles.BoxedFloat64 = newStubClass(metaClass, FloatShape, 0);
	Handles.String = newStubClass(metaClass, StringShape, 0);
	Handles.Array = newStubClass(metaClass, IndexedShape, 0);
	Handles.ByteArray = newStubClass(metaClass, BytesShape, 0);
	Handles.Association = newStubClass(metaClass, FixedShape, 2);
	Handles.Dictionary = newStubClass(metaClass, FixedShape, 2);
	Handles.OrderedCollection = newStubClass(metaClass, FixedShape, 3);
	Handles.Class = newStubClass(metaClass, FixedShape, 10);
	Handles.TypeFeedback = newStubClass(metaClass, FixedShape, 2);
	Handles.CompiledMethod = newStubClass(metaClass, CompiledCodeShape, 6);
	Handles.CompiledBlock = newStubClass(metaClass, CompiledCodeShape, 4);
	Handles.SourceCode = newStubClass(metaClass, FixedShape, 5);
	Handles.FileSourceCode = newStubClass(metaClass, FixedShape, 5);
	Handles.Block = newStubClass(metaClass, BlockShape, 3);
	Handles.Message = newStubClass(metaClass, FixedShape, 2);
	Handles.MethodContext = newStubClass(metaClass, ContextShape, 5);
	Handles.BlockContext = newStubClass(metaClass, ContextShape, 5);
	Handles.ExceptionHandler = newStubClass(metaClass, ExceptionHandlerShape, 2);
	Handles.UnwindHandler = newStubClass(metaClass, UnwindHandlerShape, 3);
	Handles.ClassNode = newStubClass(metaClass, FixedShape, 8);
	Handles.MethodNode = newStubClass(metaClass, FixedShape, 5);
	Handles.BlockNode = newStubClass(metaClass, FixedShape, 5);
	Handles.BlockScope = newStubClass(metaClass, FixedShape, 7);
	Handles.ExpressionNode = newStubClass(metaClass, FixedShape, 5);
	Handles.MessageExpressionNode = newStubClass(metaClass, FixedShape, 3);
	Handles.NilNode = newStubClass(metaClass, FixedShape, 2);
	Handles.TrueNode = newStubClass(metaClass, FixedShape, 2);
	Handles.FalseNode = newStubClass(metaClass, FixedShape, 2);
	Handles.VariableNode = newStubClass(metaClass, FixedShape, 2);
	Handles.IntegerNode = newStubClass(metaClass, FixedShape, 2);
	Handles.CharacterNode = newStubClass(metaClass, FixedShape, 2);
	Handles.SymbolNode = newStubClass(metaClass, FixedShape, 2);
	Handles.StringNode = newStubClass(metaClass, FixedShape, 2);
	Handles.ArrayNode = newStubClass(metaClass, FixedShape, 2);
	Handles.ParseError = newStubClass(metaClass, FixedShape, 3);
	Handles.UndefinedVariableError = newStubClass(metaClass, FixedShape, 2);
	Handles.RedefinitionError = newStubClass(metaClass, FixedShape, 2);
	Handles.ReadonlyVariableError = newStubClass(metaClass, FixedShape, 2);
	Handles.InvalidPragmaError = newStubClass(metaClass, FixedShape, 2);
	Handles.IoError = newStubClass(metaClass, FixedShape, 1);
	Handles.Namespace = newStubClass(metaClass, FixedShape, 3);

	Handles.nil->raw->class = Handles.UndefinedObject->raw;
	Handles.true = persistHandle(newObject(Handles.True, 0));
	Handles.false = persistHandle(newObject(Handles.False, 0));
	Handles.Smalltalk = persistHandle(newDictionary(256));
	Handles.SymbolTable = persistHandle(newArray(SYMBOL_TABLE_SIZE));
	Handles.initializeSymbol = persistHandle(getSymbol("initialize"));
	Handles.finalizeSymbol = persistHandle(getSymbol("finalize"));
	Handles.valueSymbol = persistHandle(getSymbol("value"));
	Handles.value_Symbol = persistHandle(getSymbol("value:"));
	Handles.valueValueSymbol = persistHandle(getSymbol("value:value:"));
	Handles.doesNotUnderstandSymbol = persistHandle(getSymbol("doesNotUnderstand:"));
	Handles.cannotReturnSymbol = persistHandle(getSymbol("cannotReturn:"));
	Handles.handlesSymbol = persistHandle(getSymbol("handles:"));
	Handles.generateBacktraceSymbol = persistHandle(getSymbol("generateBacktrace"));
	Handles.runHandledBySymbol = persistHandle(getSymbol("runHandledBy:"));

	// The Core namespace WRAPS the core dictionary (same identity), sits in
	// the registry under #Core, and starts as the default compile target.
	// DefaultNamespace is an indirection CELL, never a second handle to the
	// namespace object: Handles fields must stay distinct (see Handle.h).
	Handles.Namespaces = persistHandle(newDictionary(16));
	Handles.CoreNamespace = persistHandle(
		newNamespace(getSymbol("Core"), Handles.Smalltalk, newArray(0)));
	Association *defaultCell = (Association *) newObject(Handles.Association, 0);
	objectStorePtr((Object *) defaultCell, &defaultCell->raw->key,
		(Object *) getSymbol("DefaultNamespace"));
	objectStorePtr((Object *) defaultCell, &defaultCell->raw->value,
		(Object *) Handles.CoreNamespace);
	Handles.DefaultNamespace = persistHandle(defaultCell);
	symbolDictAtPutObject(Handles.Namespaces, getSymbol("Core"), (Object *) Handles.CoreNamespace);

	setGlobalObject("UndefinedObject", (Object *) Handles.UndefinedObject);
	setGlobalObject("nil", Handles.nil);
	setGlobalObject("True", (Object *) Handles.True);
	setGlobalObject("true", Handles.true);
	setGlobalObject("False", (Object *) Handles.False);
	setGlobalObject("false", Handles.false);
	setGlobalObject("SmallInteger", (Object *) Handles.SmallInteger);
	setGlobalObject("Character", (Object *) Handles.Character);
	setGlobalObject("Float", (Object *) Handles.Float);
	setGlobalObject("SmallFloat64", (Object *) Handles.SmallFloat64);
	setGlobalObject("BoxedFloat64", (Object *) Handles.BoxedFloat64);

	setGlobal("FixedShape", *(Value *) &FixedShape);
	setGlobal("FloatShape", *(Value *) &FloatShape);
	setGlobal("IndexedShape", *(Value *) &IndexedShape);
	setGlobal("StringShape", *(Value *) &StringShape);
	setGlobal("BytesShape", *(Value *) &BytesShape);
	setGlobal("CompiledCodeShape", *(Value *) &CompiledCodeShape);
	setGlobal("BlockShape", *(Value *) &BlockShape);
	setGlobal("ContextShape", *(Value *) &ContextShape);
	setGlobal("ExceptionHandlerShape", *(Value *) &ExceptionHandlerShape);
	setGlobal("UnwindHandlerShape", *(Value *) &UnwindHandlerShape);

	setGlobalObject("Symbol", (Object *) Handles.Symbol);
	setGlobalObject("String", (Object *) Handles.String);
	setGlobalObject("Array", (Object *) Handles.Array);
	setGlobalObject("ByteArray", (Object *) Handles.ByteArray);
	setGlobalObject("Association", (Object *) Handles.Association);
	setGlobalObject("Dictionary", (Object *) Handles.Dictionary);
	setGlobalObject("OrderedCollection", (Object *) Handles.OrderedCollection);
	setGlobalObject("MetaClass", (Object *) Handles.MetaClass);
	setGlobalObject("Class", (Object *) Handles.Class);
	setGlobalObject("TypeFeedback", (Object *) Handles.TypeFeedback);
	setGlobalObject("CompiledMethod", (Object *) Handles.CompiledMethod);
	setGlobalObject("CompiledBlock", (Object *) Handles.CompiledBlock);
	setGlobalObject("SourceCode", (Object *) Handles.SourceCode);
	setGlobalObject("FileSourceCode", (Object *) Handles.FileSourceCode);
	setGlobalObject("Block", (Object *) Handles.Block);
	setGlobalObject("Message", (Object *) Handles.Message);
	setGlobalObject("MethodContext", (Object *) Handles.MethodContext);
	setGlobalObject("BlockContext", (Object *) Handles.BlockContext);
	setGlobalObject("ExceptionHandler", (Object *) Handles.ExceptionHandler);
	setGlobalObject("UnwindHandler", (Object *) Handles.UnwindHandler);
	setGlobalObject("ClassNode", (Object *) Handles.ClassNode);
	setGlobalObject("MethodNode", (Object *) Handles.MethodNode);
	setGlobalObject("BlockNode", (Object *) Handles.BlockNode);
	setGlobalObject("BlockScope", (Object *) Handles.BlockScope);
	setGlobalObject("ExpressionNode", (Object *) Handles.ExpressionNode);
	setGlobalObject("MessageExpressionNode", (Object *) Handles.MessageExpressionNode);
	setGlobalObject("NilNode", (Object *) Handles.NilNode);
	setGlobalObject("TrueNode", (Object *) Handles.TrueNode);
	setGlobalObject("FalseNode", (Object *) Handles.FalseNode);
	setGlobalObject("VariableNode", (Object *) Handles.VariableNode);
	setGlobalObject("IntegerNode", (Object *) Handles.IntegerNode);
	setGlobalObject("CharacterNode", (Object *) Handles.CharacterNode);
	setGlobalObject("SymbolNode", (Object *) Handles.SymbolNode);
	setGlobalObject("StringNode", (Object *) Handles.StringNode);
	setGlobalObject("ArrayNode", (Object *) Handles.ArrayNode);
	setGlobalObject("ParseError", (Object *) Handles.ParseError);
	setGlobalObject("UndefinedVariableError", (Object *) Handles.UndefinedVariableError);
	setGlobalObject("RedefinitionError", (Object *) Handles.RedefinitionError);
	setGlobalObject("ReadonlyVariableError", (Object *) Handles.ReadonlyVariableError);
	setGlobalObject("InvalidPragmaError", (Object *) Handles.InvalidPragmaError);
	setGlobalObject("IoError", (Object *) Handles.IoError);
	setGlobalObject("Namespace", (Object *) Handles.Namespace);
	setGlobalObject("Namespaces", (Object *) Handles.Namespaces);
	setGlobalObject("SymbolTable", (Object *) Handles.SymbolTable);
	setGlobalObject("Smalltalk", (Object *) Handles.Smalltalk);

	closeHandleScope(&scope, NULL);
}


static Class *newStubClass(Class *metaClass, InstanceShape shape, size_t instanceSize)
{
	Class *class = (Class *) newStubObject(sizeof(RawClass));
	class->raw->class = metaClass->raw;
	objectStorePtr((Object *) class,  &class->raw->superClass, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->subClasses, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->methodDictionary, (Object *) Handles.nil);
	shape.varsSize = instanceSize;
	shape.size += shape.varsSize * sizeof(Value);
	class->raw->instanceShape = shape;
	objectStorePtr((Object *) class,  &class->raw->instanceVariables, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->name, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->comment, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->category, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->classVariables, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->namespace, (Object *) Handles.nil);
	return class;
}


static Class *newStubMetaClass(InstanceShape shape, size_t instanceSize)
{
	RawObject *object = (RawObject *) allocate(CurrentThread.heap, sizeof(RawMetaClass));
	object->hash = (Value) object >> 2; // XXX: replace with random hash generator
	object->payloadSize = 0;
	object->varsSize = 0;
	object->tags = 0;

	Class *metaClass = newStubClass(scopeHandle(NULL), FixedShape, 6);
	metaClass->raw->class = (RawClass *) object;

	MetaClass *class = (MetaClass *) scopeHandle(object);
	class->raw->class = metaClass->raw;
	objectStorePtr((Object *) class,  &class->raw->superClass, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->subClasses, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->methodDictionary, (Object *) Handles.nil);
	shape.varsSize = instanceSize;
	shape.size += shape.varsSize * sizeof(Value);
	class->raw->instanceShape = shape;
	objectStorePtr((Object *) class,  &class->raw->instanceVariables, (Object *) Handles.nil);
	objectStorePtr((Object *) class,  &class->raw->instanceClass, (Object *) Handles.nil);
	return (Class *) class;
}


static Object *newStubObject(size_t size)
{
	RawObject *object = (RawObject *) allocate(CurrentThread.heap, size);
	object->hash = (Value) object >> 2; // XXX: replace with random hash generator
	object->payloadSize = 0;
	object->varsSize = 0;
	object->tags = 0;
	return handle(object);
}


// ---- core manifest reader ---------------------------------------------------
// The kernel file list and its load order live in <coreDir>/package.st: the
// Core package manifest is the single source of truth, shared with the package
// tooling. The bootstrap runs before any Smalltalk exists, so this is a
// MINIMAL scanner for the two tokens the bootstrap needs, not a parser of the
// DSL: it skips "..." comments (with "" escape), consumes '...' strings (with
// '' escape), validates name: 'Core' and collects the files: #('...' ...)
// literal. Anything fancier in the manifest (computed lists, concatenation)
// is a bootstrap error by design; the manifest is ours, not adversarial input.

// Skip whitespace and "..." comments.
static void manifestSkipBlanks(const char **p, const char *end)
{
	while (*p < end) {
		char c = **p;
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			(*p)++;
		} else if (c == '"') {
			(*p)++;
			while (*p < end) {
				if (**p == '"') {
					(*p)++;
					if (*p < end && **p == '"') {
						(*p)++; // "" escape: keep scanning the comment
						continue;
					}
					break;
				}
				(*p)++;
			}
		} else {
			return;
		}
	}
}


// Read the '...' literal at *p (assumes **p == '\'') resolving the '' escape.
// Answers a malloc'd copy and advances *p past the closing quote, or NULL on
// an unterminated literal. Relies on the source buffer being NUL-terminated.
static char *manifestReadString(const char **p, const char *end)
{
	const char *scan = *p + 1;
	size_t size = 0;
	while (scan < end) {
		if (*scan == '\'') {
			if (scan[1] == '\'') {
				scan += 2;
				size++;
				continue;
			}
			break;
		}
		scan++;
		size++;
	}
	if (scan >= end) {
		return NULL;
	}
	char *result = malloc(size + 1);
	char *out = result;
	scan = *p + 1;
	for (;;) {
		if (*scan == '\'') {
			if (scan[1] == '\'') {
				*out++ = '\'';
				scan += 2;
				continue;
			}
			scan++;
			break;
		}
		*out++ = *scan++;
	}
	*out = '\0';
	*p = scan;
	return result;
}


static void manifestFreeFiles(char **files, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		free(files[i]);
	}
	free(files);
}


static _Bool manifestIsLetter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}


static _Bool manifestIsIdentifier(char c)
{
	return manifestIsLetter(c) || (c >= '0' && c <= '9');
}


// Extract the files: entries from <coreDir>/package.st. Answers a malloc'd
// array of malloc'd strings (count in *countOut), or NULL after printing the
// reason to stderr (bootstrap then fails noisily).
static char **readCoreManifestFiles(const char *coreDir, size_t *countOut)
{
	char path[strlen(coreDir) + sizeof("/package.st") + 1];
	sprintf(path, "%s/package.st", coreDir);

	FILE *file = fopen(path, "r");
	if (file == NULL) {
		fprintf(stderr, "Bootstrap: cannot open core manifest %s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	long fileSize = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (fileSize < 0) {
		fclose(file);
		fprintf(stderr, "Bootstrap: cannot read core manifest %s\n", path);
		return NULL;
	}
	char *source = malloc((size_t) fileSize + 1);
	size_t sourceSize = fread(source, 1, (size_t) fileSize, file);
	fclose(file);
	source[sourceSize] = '\0';

	const char *p = source;
	const char *end = source + sourceSize;
	char **files = NULL;
	size_t count = 0;
	size_t capacity = 0;
	_Bool nameSeen = 0;
	_Bool filesSeen = 0;
	const char *error = NULL;
	char errorBuffer[256];

	while (p < end && error == NULL) {
		manifestSkipBlanks(&p, end);
		if (p >= end) {
			break;
		}
		if (*p == '\'') {
			// a string in cascade position (version:, summary:, ...): skip it
			char *skipped = manifestReadString(&p, end);
			if (skipped == NULL) {
				error = "unterminated string";
				break;
			}
			free(skipped);
		} else if (manifestIsLetter(*p)) {
			const char *start = p;
			while (p < end && manifestIsIdentifier(*p)) {
				p++;
			}
			_Bool keyword = p < end && *p == ':';
			if (keyword) {
				p++;
			}
			size_t tokenSize = (size_t) (p - start);
			if (keyword && tokenSize == 5 && memcmp(start, "name:", 5) == 0) {
				manifestSkipBlanks(&p, end);
				if (p >= end || *p != '\'') {
					error = "name: must be followed by a plain string";
					break;
				}
				char *name = manifestReadString(&p, end);
				if (name == NULL) {
					error = "unterminated string after name:";
					break;
				}
				if (strcmp(name, "Core") != 0) {
					snprintf(errorBuffer, sizeof(errorBuffer),
						"declares name: '%s', expected 'Core' - is -b pointing at the core package?", name);
					error = errorBuffer;
					free(name);
					break;
				}
				free(name);
				nameSeen = 1;
			} else if (keyword && tokenSize == 6 && memcmp(start, "files:", 6) == 0) {
				manifestSkipBlanks(&p, end);
				if (p + 1 >= end || p[0] != '#' || p[1] != '(') {
					error = "files: must be followed by a #('...' ...) literal";
					break;
				}
				p += 2;
				for (;;) {
					manifestSkipBlanks(&p, end);
					if (p >= end) {
						error = "unterminated files: #( literal";
						break;
					}
					if (*p == ')') {
						p++;
						filesSeen = 1;
						break;
					}
					if (*p != '\'') {
						error = "files: may only contain plain '...' strings";
						break;
					}
					char *entry = manifestReadString(&p, end);
					if (entry == NULL) {
						error = "unterminated string in files:";
						break;
					}
					if (count == capacity) {
						capacity = capacity == 0 ? 160 : capacity * 2;
						files = realloc(files, capacity * sizeof(char *));
					}
					files[count++] = entry;
				}
			}
		} else {
			p++; // any other char (#, (, ), ;, digits, ...) is not ours to check
		}
	}

	free(source);
	if (error == NULL && !nameSeen) {
		error = "missing name: 'Core'";
	}
	if (error == NULL && (!filesSeen || count == 0)) {
		error = "missing or empty files: list";
	}
	if (error != NULL) {
		fprintf(stderr, "Bootstrap: bad core manifest %s: %s\n", path, error);
		manifestFreeFiles(files, count);
		return NULL;
	}
	*countOut = count;
	return files;
}


static _Bool parseKernelFiles(char *coreDir)
{
	size_t kernelFileCount = 0;
	char **kernelFiles = readCoreManifestFiles(coreDir, &kernelFileCount);
	if (kernelFiles == NULL) {
		return 0;
	}

	HandleScope scope;
	openHandleScope(&scope);

	Array *classInstanceVariables = newArray(classGetInstanceShape(Handles.Class).varsSize);
	arrayAtPutObject(classInstanceVariables, 0, (Object *) asString("superClass"));
	arrayAtPutObject(classInstanceVariables, 1, (Object *) asString("subClasses"));
	arrayAtPutObject(classInstanceVariables, 2, (Object *) asString("methodDictionary"));
	arrayAtPutObject(classInstanceVariables, 3, (Object *) asString("instanceShape"));
	arrayAtPutObject(classInstanceVariables, 4, (Object *) asString("instanceVariables"));
	arrayAtPutObject(classInstanceVariables, 5, (Object *) asString("name"));
	arrayAtPutObject(classInstanceVariables, 6, (Object *) asString("comment"));
	arrayAtPutObject(classInstanceVariables, 7, (Object *) asString("category"));
	arrayAtPutObject(classInstanceVariables, 8, (Object *) asString("classVariables"));
	arrayAtPutObject(classInstanceVariables, 9, (Object *) asString("namespace"));
	classSetInstanceVariables(Handles.Class, classInstanceVariables);

	size_t coreDirNameSize = strlen(coreDir);
	for (size_t i = 0; i < kernelFileCount; i++) {
		size_t fileNameSize = strlen(kernelFiles[i]);
		char fileName[coreDirNameSize + fileNameSize + 2];
		memcpy(fileName, coreDir, coreDirNameSize);
		fileName[coreDirNameSize] = '/';
		memcpy(fileName + coreDirNameSize + 1, kernelFiles[i], fileNameSize + 1);
		if (!parseFile(fileName, NULL, NULL)) {
			closeHandleScope(&scope, NULL);
			manifestFreeFiles(kernelFiles, kernelFileCount);
			return 0;
		}
	}
	manifestFreeFiles(kernelFiles, kernelFileCount);

	Iterator iterator;
	initDictIterator(&iterator, Handles.Smalltalk);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope2;
		openHandleScope(&scope2);
		Association *assoc = (Association *) iteratorNextObject(&iterator);
		if (!isNil(assoc) && valueTypeOf(assoc->raw->value, VALUE_POINTER)) {
			Object *object = scopeHandle(asObject(assoc->raw->value));
			ASSERT(object->raw->class != NULL);
			if (object->raw->class->class == Handles.MetaClass->raw) {
				invokeInititalize(object);
			}
		}
		closeHandleScope(&scope2, NULL);
	}

	// Layout drift guards for VM-known objects: the C struct, the bootstrap
	// stub size and the .st mirror must agree. The mirror's ivar count landed
	// in instanceShape.varsSize when its class definition rebuilt the stub.
	ASSERT(classGetInstanceShape(Handles.Namespace).varsSize
		== (sizeof(RawNamespace) - HEADER_SIZE) / sizeof(Value));
	ASSERT(classGetInstanceShape(Handles.Class).varsSize
		== (sizeof(RawClass) - HEADER_SIZE) / sizeof(Value));
	ASSERT(classGetInstanceShape(Handles.BlockScope).varsSize
		== (sizeof(RawBlockScope) - HEADER_SIZE) / sizeof(Value));
	ASSERT(classGetInstanceShape(Handles.ClassNode).varsSize
		== (sizeof(RawClassNode) - HEADER_SIZE) / sizeof(Value));

	closeHandleScope(&scope, NULL);
	return 1;
}
