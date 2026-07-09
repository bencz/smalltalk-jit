#include "Entry.h"
#include "Lookup.h"
#include "Handle.h"
#include "Class.h"
#include "Lookup.h"
#include "StubCode.h"
#include "Compiler.h"
#include "Iterator.h"
#include "Heap.h"
#include "Smalltalk.h"
#include "Thread.h"
#include <string.h>
#include <errno.h>

static void initArgs(Value *rawArgs, EntryArgs *args);
static void patchMethodNode(MethodNode *method);
static Value evalBlockNode(BlockNode *block);


Value invokeMethod(CompiledMethod *method, EntryArgs *args)
{
    HandleScope scope;
    openHandleScope(&scope);

    Class *class = scopeHandle(args->values[0].isHandle ? args->values[0].handle->raw->class : getClassOf(args->values[0].value));

    union PointerConverter converter;
    converter.object_pointer = getStubNativeCode(&SmalltalkEntry)->insts;
    NativeCodeEntry entry = converter.function_pointer;

    Value rawArgs[args->size];
    initArgs(rawArgs, args);
    initThreadContext(&CurrentThread);
    Value result = entry(method->raw, getNativeCode(class, method)->insts, rawArgs, &CurrentThread);

    closeHandleScope(&scope, NULL);
    return result;
}


Value invokeInititalize(Object *object)
{
	Dictionary *methods = scopeHandle(asObject(object->raw->class->methodDictionary));
	Object *init = symbolDictObjectAt(methods, Handles.initializeSymbol);
	if (!isNil(init)) {
		EntryArgs args = { .size = 0 };
		entryArgsAddObject(&args, object);
		return invokeMethod((CompiledMethod *) init, &args);
	}
	return getTaggedPtr(object);
}


Value sendMessage(String *selector, EntryArgs *args)
{
    union PointerConverter converter;
    converter.object_pointer = getStubNativeCode(&SmalltalkEntry)->insts;
    NativeCodeEntry entry = converter.function_pointer;

    RawClass *class = args->values[0].isHandle ? args->values[0].handle->raw->class : getClassOf(args->values[0].value);

    NativeCodeEntry nativeCodeEntry = cachedLookupNativeCode(class, selector->raw);

    converter.function_pointer = nativeCodeEntry;
    NativeCode *nativeCode = (NativeCode *) ((uint8_t *) converter.object_pointer - offsetof(NativeCode, insts));

    Value rawArgs[args->size];
    initArgs(rawArgs, args);
    initThreadContext(&CurrentThread);
    return entry(nativeCode->compiledCode, nativeCodeEntry, rawArgs, &CurrentThread);
}

static void initArgs(Value *rawArgs, EntryArgs *args)
{
	for (size_t i = 0; i < args->size; i++) {
		EntryArg *arg = &args->values[i];
		rawArgs[i] = arg->isHandle ? getTaggedPtr(arg->handle) : arg->value;
	}
}


Value evalCode(char *source)
{
	HandleScope scope;
	openHandleScope(&scope);

	Value error;
	size_t sourceSize = strlen(source);
	char *eval = malloc(sourceSize + 7); // +7 for "eval[]" and \0
	Parser parser;
	Value result;

	memcpy(eval, "eval[", 5);
	memcpy(eval + 5, source, sourceSize);
	eval[sourceSize + 5] = ']';
	eval[sourceSize + 6] = '\0';

	initParser(&parser, asString(eval));
	MethodNode *node = parseMethod(&parser);
	if (node == NULL) {
		printParseError(&parser, eval);
		return tagInt(1);
	}

	patchMethodNode(node);
	Object *method = compileMethod(node, Handles.UndefinedObject);
	if (method->raw->class == Handles.CompiledMethod->raw) {
		EntryArgs args = { .size = 0 };
		entryArgsAddObject(&args, Handles.nil);
		result = invokeMethod((CompiledMethod *) method, &args);
	} else {
		printCompileError((CompileError *) method);
		result = tagInt(1);
	}

	freeParser(&parser);
	free(eval);

	EntryArgs args = { .size = 0 };
	if (valueTypeOf(result, VALUE_POINTER)) {
		entryArgsAddObject(&args, scopeHandle(asObject(result)));
	} else {
		entryArgsAdd(&args, result);
	}
	sendMessage(getSymbol("printNl"), &args);

	closeHandleScope(&scope, NULL);
	return valueTypeOf(result, VALUE_INT) ? result : tagInt(0);
}


// Like evalCode, but RETURNS the doit's result value (rooted in the caller's
// handle scope) instead of printing it — for C callers that need the object.
Value evalObject(char *source)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t sourceSize = strlen(source);
	char *eval = malloc(sourceSize + 7);
	memcpy(eval, "eval[", 5);
	memcpy(eval + 5, source, sourceSize);
	eval[sourceSize + 5] = ']';
	eval[sourceSize + 6] = '\0';

	Parser parser;
	initParser(&parser, asString(eval));
	MethodNode *node = parseMethod(&parser);
	Value result = tagInt(0);
	if (node != NULL) {
		patchMethodNode(node);
		Object *method = compileMethod(node, Handles.UndefinedObject);
		if (method->raw->class == Handles.CompiledMethod->raw) {
			EntryArgs args = { .size = 0 };
			entryArgsAddObject(&args, Handles.nil);
			result = invokeMethod((CompiledMethod *) method, &args);
		}
	}
	freeParser(&parser);
	free(eval);

	if (valueTypeOf(result, VALUE_POINTER)) {
		return getTaggedPtr(closeHandleScope(&scope, scopeHandle(asObject(result))));
	}
	closeHandleScope(&scope, NULL);
	return result;
}


static void patchMethodNode(MethodNode *method)
{
	OrderedCollection *expressions = blockNodeGetExpressions(methodNodeGetBody(method));
	size_t size = ordCollSize(expressions);
	if (size > 0) {
		expressionNodeEnableReturn((ExpressionNode *) ordCollObjectAt(expressions, size - 1));
	}
}


_Bool parseFileAndInitialize(char *filename, Value *lastBlockResult)
{
	HandleScope scope;
	openHandleScope(&scope);

	OrderedCollection *classes = newOrdColl(8);
	OrderedCollection *blocks = newOrdColl(8);
	if (!parseFile(filename, classes, blocks)) {
		return 0;
	}

	// Iterate by index, re-fetching each element from the (rooted) collection.
	// invokeInititalize / evalBlockNode run arbitrary Smalltalk code that can
	// scavenge — a raw-pointer Iterator would dangle when the backing array moves,
	// leaving the NEXT element a stale pointer. ordCollObjectAt re-reads the live
	// collection and returns a fresh scope handle, so it survives GC mid-loop.
	// Nested scope per item: each iteration allocates handles (ordCollObjectAt plus the
	// initialize/block evaluation), which would otherwise pile up in this outer scope and
	// overflow it once a file defines enough classes.
	size_t classesSize = ordCollSize(classes);
	for (size_t i = 0; i < classesSize; i++) {
		HandleScope inner;
		openHandleScope(&inner);
		invokeInititalize(ordCollObjectAt(classes, i));
		closeHandleScope(&inner, NULL);
	}

	size_t blocksSize = ordCollSize(blocks);
	for (size_t i = 0; i < blocksSize; i++) {
		HandleScope inner;
		openHandleScope(&inner);
		*lastBlockResult = evalBlockNode((BlockNode *) ordCollObjectAt(blocks, i));
		closeHandleScope(&inner, NULL);
	}

	closeHandleScope(&scope, NULL);
	return 1;
}


_Bool parseFile(char *filename, OrderedCollection *classes, OrderedCollection *blocks)
{
	HandleScope scope;
	openHandleScope(&scope);

	FILE *file = fopen(filename, "r");
	Parser parser;

	if (file == NULL) {
		printf("Cannot open file '%s' (errno: %i)\n", filename, errno);
		closeHandleScope(&scope, NULL);
		return 0;
	}
	initFileParser(&parser, file, asString(filename));

	while (!parserAtEnd(&parser)) {
		// A nested scope PER top-level item: each parsed node/class survives via the
		// outer-rooted classes/blocks collection, so releasing the item's transient
		// parse/compile handles here keeps the file-wide scope from overflowing (~3
		// handles/class would otherwise abort scopeHandle at 256, i.e. ~85 classes).
		HandleScope inner;
		openHandleScope(&inner);
		if (blocks != NULL && currentToken(&parser.tokenizer)->type == TOKEN_OPEN_SQUARE_BRACKET) {
			BlockNode *node = parseBlock(&parser);
			if (node == NULL) {
				printParseError(&parser, filename);
				closeHandleScope(&inner, NULL);
				closeHandleScope(&scope, NULL);
				return 0;
			}
			ordCollAddObject(blocks, (Object *) node);
		} else {
			ClassNode *node = parseClass(&parser);
			if (node == NULL) {
				printParseError(&parser, filename);
				closeHandleScope(&inner, NULL);
				closeHandleScope(&scope, NULL);
				return 0;
			}

			Object *class = buildClass(node);
			if (isCompileError(class)) {
				printCompileError((CompileError *) class);
				closeHandleScope(&inner, NULL);
				closeHandleScope(&scope, NULL);
				return 0;
			}

			if (classes != NULL) {
				ordCollAddObject(classes, class);
			}
		}
		closeHandleScope(&inner, NULL);
	}

	freeParser(&parser);
	fclose(file);
	closeHandleScope(&scope, NULL);
	return 1;
}


// Like parseFile, but the program text comes from a source STRING rather than a
// file. Used to run a spawned isolate's entry program (which may define classes
// and then run top-level blocks): source is portable across isolates, a live
// block is not.
_Bool parseSource(char *source, OrderedCollection *classes, OrderedCollection *blocks)
{
	HandleScope scope;
	openHandleScope(&scope);

	Parser parser;
	initParser(&parser, asString(source));

	while (!parserAtEnd(&parser)) {
		// Nested scope per top-level item (see parseFile) so the whole-source scope
		// does not overflow on a source string that defines many classes.
		HandleScope inner;
		openHandleScope(&inner);
		if (blocks != NULL && currentToken(&parser.tokenizer)->type == TOKEN_OPEN_SQUARE_BRACKET) {
			BlockNode *node = parseBlock(&parser);
			if (node == NULL) {
				printParseError(&parser, source);
				closeHandleScope(&inner, NULL);
				closeHandleScope(&scope, NULL);
				return 0;
			}
			ordCollAddObject(blocks, (Object *) node);
		} else {
			ClassNode *node = parseClass(&parser);
			if (node == NULL) {
				printParseError(&parser, source);
				closeHandleScope(&inner, NULL);
				closeHandleScope(&scope, NULL);
				return 0;
			}
			Object *class = buildClass(node);
			if (isCompileError(class)) {
				printCompileError((CompileError *) class);
				closeHandleScope(&inner, NULL);
				closeHandleScope(&scope, NULL);
				return 0;
			}
			if (classes != NULL) {
				ordCollAddObject(classes, class);
			}
		}
		closeHandleScope(&inner, NULL);
	}

	freeParser(&parser);
	closeHandleScope(&scope, NULL);
	return 1;
}


_Bool parseSourceAndInitialize(char *source, Value *lastBlockResult)
{
	HandleScope scope;
	openHandleScope(&scope);

	OrderedCollection *classes = newOrdColl(8);
	OrderedCollection *blocks = newOrdColl(8);
	if (!parseSource(source, classes, blocks)) {
		closeHandleScope(&scope, NULL);
		return 0;
	}

	size_t classesSize = ordCollSize(classes);
	for (size_t i = 0; i < classesSize; i++) {
		HandleScope inner;
		openHandleScope(&inner);
		invokeInititalize(ordCollObjectAt(classes, i));
		closeHandleScope(&inner, NULL);
	}

	size_t blocksSize = ordCollSize(blocks);
	for (size_t i = 0; i < blocksSize; i++) {
		HandleScope inner;
		openHandleScope(&inner);
		*lastBlockResult = evalBlockNode((BlockNode *) ordCollObjectAt(blocks, i));
		closeHandleScope(&inner, NULL);
	}

	closeHandleScope(&scope, NULL);
	return 1;
}


static Value evalBlockNode(BlockNode *block)
{
	HandleScope scope;
	openHandleScope(&scope);

	Value result;
	MethodNode *node = newObject(Handles.MethodNode, 0);
	methodNodeSetSelector(node, asString("eval"));
	methodNodeSetPragmas(node, newOrdColl(0));
	methodNodeSetBody(node, block);
	methodNodeSetSourceCode(node, blockNodeGetSourceCode(block));

	Object *method = compileMethod(node, Handles.UndefinedObject);
	if (method->raw->class == Handles.CompiledMethod->raw) {
		EntryArgs args = { .size = 0 };
		entryArgsAddObject(&args, Handles.nil);
		result = invokeMethod((CompiledMethod *) method, &args);
	} else {
		printCompileError((CompileError *) method);
		result = getTaggedPtr(Handles.nil);
	}

	closeHandleScope(&scope, NULL);
	return result;
}
