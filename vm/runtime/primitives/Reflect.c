// The reflective compiler: parsing and class building, driven from Smalltalk.
//
// This is what `st build` stands on. A package is loaded by PackageLoader,
// which is written IN Smalltalk (packages/Core/src/Packages/), so the image has
// to be able to read a .st file and turn it into classes from the inside. The
// three parse primitives answer AST nodes, which are ordinary heap objects of
// classes the image already has (ClassNode, MethodNode, ...), and the two build
// primitives hand them back to the same class builder that `-b` uses.
//
// ONE PATH, NOT TWO. Building a class here goes through classBuild and
// compiling a method goes through classCompileMethodInto, which are exactly
// what tools/ClassBuilder.c uses for `st -b packages/Core`. A second
// implementation would be a second answer to questions like "where does a class
// variable live", and that one has already been got wrong once in this project,
// silently (see the Character/Table note in docs/jit-v2/01-gate.md).
//
// AN ERROR IS AN ANSWER HERE, not a failure. The kernel's wrappers read
//
//     result := self basicParseClass.
//     (result isKindOf: Exception) ifTrue: [result signal].
//
// so a parse error is RETURNED as a ParseError object and signalled in
// Smalltalk, where it can be caught. Returning PRIMITIVE_FAILED instead would
// run the empty fallback and abort with `primitiveFailed:`, which is the one
// thing a compiler must not do to a syntax error in someone's source file.

#include "runtime/primitives/Shared.h"
#include "compiler/Ast.h"
#include "compiler/Parser.h"
#include "jit/CompiledMethod.h"
#include "core/Smalltalk.h"
#include "os/OsFile.h"
#include "runtime/String.h"
#include "core/Namespace.h"
#include "tools/ClassBuilder.h"
#include <stdio.h>
#include <unistd.h>


// Is this heap object an instance of the named global class? Used to check an
// AST node arriving from Smalltalk, where nothing stops a caller passing a
// String to `buildClass:`.
static _Bool isNodeOfClass(Value value, Class *class)
{
	return valueTypeOf(value, VALUE_POINTER) && class != NULL && class->raw != NULL
		&& rawObjectClassIndex(asObject(value)) == classIndexOf(class);
}


// Every exception in this kernel inherits `messageText` as its FIRST instance
// variable (Exception.st declares `| messageText |` and nothing above it does),
// which is the same layout compiler/Parser.h states for RawParseError. Named
// here so the assumption is written down where it is used rather than being a
// bare slot index.
typedef struct {
	OBJECT_HEADER;
	Value messageText;
} RawExceptionHead;


// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
//
// The Smalltalk Parser holds `stream`, `source` and `atEnd`, and NOTHING that
// names a position. So the C Parser is rebuilt from it on every call and its
// stopping point is written back into the stream afterwards; the file
// descriptor IS the position.
//
// That is why a STREAM-backed parser can read a file class by class and a
// STRING-backed one cannot go past the first item: a String has no cursor to
// advance. The old VM had the same shape with a `TODO: preserve position` where
// this comment is, and the same limitation, unremarked. It is written down here
// instead, because `Parser parseString:` followed by two parseClass calls
// answers the FIRST class twice and that is a wrong answer, not a failure.
// PackageLoader uses the stream form, which is why it works.

typedef struct {
	Parser parser;
	FILE *file;        // NULL for a string-backed parse
	OsFd descriptor;   // the stream's descriptor, when there is one
} ReflectiveParse;


// Rebuild a C parser from the Smalltalk one. 0 when the receiver is not a
// Parser or the stream cannot be opened.
static _Bool parseBegin(Value receiver, ReflectiveParse *state)
{
	if (!valueTypeOf(receiver, VALUE_POINTER)) {
		return 0;
	}
	RawParserObject *object = (RawParserObject *) asObject(receiver);
	if (!valueTypeOf(object->source, VALUE_POINTER)) {
		return 0;
	}
	String *source = (String *) scopeHandle(asObject(object->source));

	state->file = NULL;
	state->descriptor = OS_FD_INVALID;
	// "HAS A STREAM" IS NOT "IS A POINTER". `Parser parseString:` never assigns
	// the slot, so it holds NIL -- and nil IS a pointer, so a bare VALUE_POINTER
	// test took the stream branch for every string-backed parse and sent
	// `descriptor` to nil. That is the two-spellings-of-empty rule again: the
	// allocator's zero means ABSENT and nil means "the Smalltalk object nil",
	// and a slot the image has written to holds the second one.
	_Bool hasStream = valueTypeOf(object->stream, VALUE_POINTER)
		&& asObject(object->stream) != Handles.nil.raw;
	if (hasStream) {
		// The descriptor comes from SENDING `descriptor`, not from reaching into
		// the stream's slots: it is declared by ExternalStream, four
		// superclasses below FileStream, so a C-side slot index would be a
		// second copy of a layout that Smalltalk already owns.
		//
		// It is DUPLICATED before being wrapped in a FILE*, because fclose on
		// that FILE* must not close the descriptor the Smalltalk stream owns.
		_Bool understood = 0;
		Value fdValue = jitSendUnary(object->stream, "descriptor", &understood);
		if (!understood || !valueTypeOf(fdValue, VALUE_INT)) {
			return 0;
		}
		state->descriptor = (OsFd) asCInt(fdValue);
		int duplicate = dup((int) state->descriptor);
		if (duplicate < 0) {
			return 0;
		}
		state->file = fdopen(duplicate, "r");
		if (state->file == NULL) {
			close(duplicate);
			return 0;
		}
		initFileParser(&state->parser, state->file, source);
		return 1;
	}
	initParser(&state->parser, source);
	return 1;
}


// Write `atEnd` back, and leave the shared descriptor exactly where this parse
// stopped so the next call continues from there.
static void parseEnd(Value receiver, ReflectiveParse *state)
{
	RawParserObject *object = (RawParserObject *) asObject(receiver);
	// atEnd is a plain Smalltalk boolean and the booleans are immortal, so this
	// needs no write barrier.
	object->atEnd = tagPtr(parserAtEnd(&state->parser)
		? Handles.true_.raw : Handles.false_.raw);

	if (state->file != NULL) {
		size_t restart = currentToken(&state->parser.tokenizer)->position - 1;
		// fclose FIRST, then seek. The FILE* wraps a dup of the stream's
		// descriptor, so the two SHARE one file offset, and closing a read FILE*
		// seeks that shared offset back to its own buffered logical position.
		// Restoring before the fclose lets that sync silently clobber the
		// restart position, which breaks the SECOND parseClass of any
		// multi-class file. Measured in the old VM; kept in this order for the
		// same reason.
		fclose(state->file);
		osFileSetPosition(state->descriptor, (int64_t) restart);
	}
	freeParser(&state->parser);
}


// The ParseError the kernel signals. Built here rather than in Smalltalk
// because only the C parser knows the token and the source position.
static Value parseErrorFrom(Parser *parser)
{
	Class *parseErrorClass = getClass("ParseError");
	if (parseErrorClass == NULL) {
		return PRIMITIVE_FAILED;
	}
	HandleScope scope;
	openHandleScope(&scope);
	ParseError *error = newObject(parseErrorClass, 0);
	Token *token = currentToken(&parser->tokenizer);
	// messageText stays nil: ParseError>>defaultMessageText builds the sentence
	// from the token and the position, which is where that wording belongs.
	rawObjectStorePtr((RawObject *) error->raw, &error->raw->token,
		(RawObject *) stringFromC(token->content)->raw);
	rawObjectStorePtr((RawObject *) error->raw, &error->raw->sourceCode,
		(RawObject *) createSourceCode(parser, 1)->raw);
	Value answer = objectTagged((Object *) error);
	closeHandleScope(&scope, NULL);
	return answer;
}


typedef enum { PARSE_CLASS, PARSE_METHOD, PARSE_METHOD_OR_BLOCK } ParseWhat;


static Value reflectiveParse(Value *args, uint64_t argc, ParseWhat what)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// Parsing allocates every node it builds, so the caller's frames are
	// anchored for the whole of it.
	PRIMITIVE_ALLOCATES(args);
	// AND A HANDLE SCOPE, because parseBegin takes handles on the source String
	// and the parser holds them across every allocation the parse makes. Without
	// one, scopeHandle has no scope to register in and the parser reads a String
	// that the first node allocation may already have moved.
	HandleScope scope;
	openHandleScope(&scope);
	Value answer;
	ReflectiveParse state;
	if (!parseBegin(primitiveReceiver(args), &state)) {
		closeHandleScope(&scope, NULL);
		PRIMITIVE_DONE_ALLOCATING();
		return PRIMITIVE_FAILED;
	}

	Object *node;
	switch (what) {
	case PARSE_CLASS:
		node = (Object *) parseClass(&state.parser);
		break;
	case PARSE_METHOD:
		node = (Object *) parseMethod(&state.parser);
		break;
	default:
		node = currentToken(&state.parser.tokenizer)->type
				== TOKEN_OPEN_SQUARE_BRACKET
			? (Object *) parseBlock(&state.parser)
			: (Object *) parseMethod(&state.parser);
		break;
	}
	answer = node == NULL ? parseErrorFrom(&state.parser) : objectTagged(node);
	// The receiver is read FRESH: the parse allocated, so a collection may have
	// moved the Parser object since this primitive was entered.
	parseEnd(primitiveReceiver(args), &state);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primParseClass(Value *args, uint64_t argc)
{
	return reflectiveParse(args, argc, PARSE_CLASS);
}


Value primParseMethod(Value *args, uint64_t argc)
{
	return reflectiveParse(args, argc, PARSE_METHOD);
}


Value primParseMethodOrBlock(Value *args, uint64_t argc)
{
	return reflectiveParse(args, argc, PARSE_METHOD_OR_BLOCK);
}


// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// A build failure as an Error the kernel can signal, carrying the words the
// class builder produced. Same rule as a parse error: an answer, not a failure.
static Value buildErrorFrom(const ClassBuildError *error)
{
	// The class the builder NAMED, when it named one. `on: RedefinitionError
	// do: [...]` in the image cannot work if every build failure arrives as
	// Error, and the message string is not something a handler can match on.
	// A name the image does not have falls back to Error rather than failing:
	// reporting the wrong class of error still reports the error.
	Class *errorClass = error->errorClass != NULL
		? getClass((char *) error->errorClass) : NULL;
	if (errorClass == NULL) {
		errorClass = getClass("Error");
	}
	if (errorClass == NULL) {
		return PRIMITIVE_FAILED;
	}
	HandleScope scope;
	openHandleScope(&scope);
	Object *raised = newObject(errorClass, 0);
	char text[512];
	if (error->what != NULL) {
		char name[256];
		stringPrintOn(error->what, name);
		snprintf(text, sizeof text, "%s: %s", error->message, name);
	} else {
		snprintf(text, sizeof text, "%s", error->message);
	}
	// Through the barrier: the String is young and the Error may already have
	// been promoted.
	rawObjectStorePtr((RawObject *) raised->raw,
		&((RawExceptionHead *) raised->raw)->messageText,
		(RawObject *) stringFromC(text)->raw);
	Value answer = objectTagged(raised);
	closeHandleScope(&scope, NULL);
	return answer;
}


// What a build answers, once. Three outcomes and not two.
//
// THE THIRD ONE IS WHY THIS EXISTS: a build can succeed and define NO CLASS.
// `Name := Namespace [ ... ]` is a container of definitions, so there is no
// single class to answer, and classBuildIn says so by answering NULL with no
// error. Both primitives used to tag that NULL, and objectTagged dereferences
// its argument, so declaring a namespace SEGFAULTED the VM. nil is the right
// answer and the image is already written for it: PackageLoader and
// Compiler>>compileStream: both skip the answer when `node isNamespace`.
static Value buildAnswer(Class *class, const ClassBuildError *error)
{
	if (error->message != NULL) {
		return buildErrorFrom(error);
	}
	return class == NULL ? tagPtr(Handles.nil.raw) : objectTagged((Object *) class);
}


Value primBuildClass(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value nodeValue = primitiveArgument(args, 0);
	if (!isNodeOfClass(nodeValue, &Handles.ClassNode)) {
		return PRIMITIVE_FAILED; // not a ClassNode: the caller's bug, said loudly
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	ClassNode *node = scopeHandle(asObject(primitiveArgument(args, 0)));
	ClassBuildError error = CLASS_BUILD_ERROR_NONE;
	Class *class = classBuild(node, &error);
	Value answer = buildAnswer(class, &error);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// The namespace argument of the `...in:` forms. nil means CORE, which is the
// same NULL convention core/Namespace.h uses everywhere, so a package that
// declares no namespace behaves exactly as before namespaces existed.
static _Bool namespaceArgument(Value value, Namespace **answer)
{
	*answer = NULL;
	if (!valueTypeOf(value, VALUE_POINTER) || asObject(value) == Handles.nil.raw) {
		return 1;
	}
	Class *namespaceClass = getClass("Namespace");
	if (namespaceClass == NULL || !isNodeOfClass(value, namespaceClass)) {
		return 0;
	}
	*answer = scopeHandle(asObject(value));
	return 1;
}


// Compiler>>basicBuildClass: aClassNode in: aNamespace
Value primBuildClassIn(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	if (!isNodeOfClass(primitiveArgument(args, 0), &Handles.ClassNode)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Namespace *namespace;
	Value answer;
	if (!namespaceArgument(primitiveArgument(args, 1), &namespace)) {
		answer = PRIMITIVE_FAILED;
	} else {
		ClassNode *node = scopeHandle(asObject(primitiveArgument(args, 0)));
		ClassBuildError error = CLASS_BUILD_ERROR_NONE;
		Class *class = classBuildIn(node, namespace, &error);
		answer = buildAnswer(class, &error);
	}
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Compiler>>basicCompileMethod: aMethodNode in: aClass namespace: aNamespace
Value primCompileMethodIn(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	if (!isNodeOfClass(primitiveArgument(args, 0), &Handles.MethodNode)
			|| receiverAsClass(primitiveArgument(args, 1)) == NULL) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Namespace *namespace;
	Value answer;
	if (!namespaceArgument(primitiveArgument(args, 2), &namespace)) {
		answer = PRIMITIVE_FAILED;
	} else {
		MethodNode *node = scopeHandle(asObject(primitiveArgument(args, 0)));
		Class *target = receiverAsClass(primitiveArgument(args, 1));
		ClassBuildError error = CLASS_BUILD_ERROR_NONE;
		CompiledMethod *method =
			classCompileMethodInto(node, target, NULL, namespace, &error);
		answer = error.message != NULL
			? buildErrorFrom(&error)
			: objectTagged((Object *) method);
	}
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// The session default namespace
// ---------------------------------------------------------------------------
//
// `Namespace default` is what a class build without an explicit namespace
// compiles against, and ProjectTool sets it to the project's own namespace
// before loading the package graph. It has to survive across primitive calls
// and across a SNAPSHOT, so the question is where the VM keeps it.
//
// IN THE GLOBALS DICTIONARY, under a reserved name, and not in a new VM field.
// The old VM held it in a dedicated handle; that needs a root provider so the
// collector keeps it, and a line in the snapshot writer and reader so an image
// does not lose it -- and a field the writer persists and the reader drops is
// exactly the failure the level 11 fixpoint check exists to catch. The globals
// dictionary is ALREADY a root and ALREADY written to the image, so putting it
// there is one line instead of three seams, and it cannot be forgotten by
// either half.
//
// These two live in this file rather than a domain of their own because they
// are compile-time scope: the same reason the reflective compiler is here.
#define DEFAULT_NAMESPACE_GLOBAL "DefaultNamespace"

Value primDefaultNamespace(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value current = getGlobal(DEFAULT_NAMESPACE_GLOBAL);
	// Unset means nothing has chosen one yet, and that is not an error: the
	// kernel's own comment says the default is Core until a project image
	// changes it. Failing would run the empty fallback and abort.
	return valueTypeOf(current, VALUE_POINTER) ? current : tagPtr(Handles.nil.raw);
}


Value primSetDefaultNamespace(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value namespace = primitiveArgument(args, 0);
	Class *namespaceClass = getClass("Namespace");
	// A non-Namespace FAILS, and here that is right: the kernel's fallback is
	// `InvalidArgumentError signal:`, which is a better answer than anything
	// this can produce.
	if (namespaceClass == NULL || !isNodeOfClass(namespace, namespaceClass)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args); // interning the name may allocate
	setGlobal(DEFAULT_NAMESPACE_GLOBAL, primitiveArgument(args, 0));
	Value answer = primitiveReceiver(args);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Behavior>>removeSelector: aSymbol
//
// The receiver's OWN dictionary, which is what the kernel's comment says and is
// the only thing that can be undone: an inherited method is not the receiver's
// to remove, and "removing" one by shadowing it would be an addition wearing the
// wrong name.
//
// FAILS when there is no such selector, rather than answering. The kernel is
// written for that -- the fallback signals a NotFoundError -- and it is the
// right split: a selector that is not here is a program error, and answering
// self would let `removeSelector:` on a typo look like success.
//
// AND IT FLUSHES THE SEND CACHES. A warm site holds the NativeCode it dispatched
// to last time, so without this the removed method keeps being called from
// exactly the sites that ran often enough to matter.
Value primClassRemoveSelector(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Class *class = receiverAsClass(primitiveReceiver(args));
	Value selector = primitiveArgument(args, 0);
	// TEXT, not merely a pointer. `nil` IS a pointer, so a bare VALUE_POINTER
	// test hands nil to asSymbol, which reads a byte count out of an
	// UndefinedObject and interns a Symbol of whatever length it finds. That is
	// the same family as reading an immediate through asObject, and it is
	// checked here for the same reason: the argument comes from Smalltalk and
	// nothing above stops a caller passing anything at all.
	if (class == NULL || !valueTypeOf(selector, VALUE_POINTER)
			|| rawObjectFormat(asObject(selector)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	// asSymbol ALLOCATES when the argument is a String rather than an interned
	// Symbol, so the frame is anchored before it and not after.
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	String *name = scopeHandle(asObject(primitiveArgument(args, 0)));
	Value methods = class->raw->methodDictionary;
	_Bool removed = 0;
	if (valueTypeOf(methods, VALUE_POINTER)) {
		removed = symbolDictRemove(scopeHandle(asObject(methods)), asSymbol(name));
	}
	if (removed) {
		jitFlushSendCaches();
	}
	Value answer = removed ? primitiveReceiver(args) : PRIMITIVE_FAILED;
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primCompileMethod(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	if (!isNodeOfClass(primitiveArgument(args, 0), &Handles.MethodNode)
			|| receiverAsClass(primitiveArgument(args, 1)) == NULL) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	MethodNode *node = scopeHandle(asObject(primitiveArgument(args, 0)));
	Class *target = receiverAsClass(primitiveArgument(args, 1));
	ClassBuildError error = CLASS_BUILD_ERROR_NONE;
	// classVariableScope NULL, meaning "same as the target". The caller named
	// the class it wants the method in, exactly as the old VM's form did, and
	// nothing here can second-guess which side it meant.
	CompiledMethod *method = classCompileMethodInto(node, target, NULL, NULL, &error);
	Value answer = error.message != NULL
		? buildErrorFrom(&error)
		: objectTagged((Object *) method);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
