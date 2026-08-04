// JSON, over the C runtime in runtime/Json.c.
//
// THE RUNTIME WAS ALREADY HERE, exactly as BigInt was: a strict RFC 8259 parser
// and an encoder that walks a core-typed graph by raw pointers. What was
// missing were these two primitives, so `Json parse:` and `Json encode:` ran
// the Smalltalk implementation, which is the reference and the validating
// fallback but not the fast path.
//
// MEASURED, on the 1 MB string that tests/JsonTest.st's GC-stress section
// builds: the Smalltalk parse took 7685 ms and the encode 4737 ms, which is why
// that file did not finish inside the suite's 120-second timeout.
//
// FAILING IS PART OF THE DESIGN HERE, not an error path. The C parser refuses a
// syntax error, an integer past SmallInteger and nesting past JSON_MAX_DEPTH;
// the C encoder refuses any class outside the core set, a NaN or an infinity,
// and the same depth. Every one of those falls through to the Smalltalk method,
// which either produces a precise JsonParseError, promotes to a LargeInteger,
// or walks that level reflectively and re-enters this fast path for the core
// subtrees underneath. That is why neither primitive tries to explain itself:
// the kernel is what explains.

#include "runtime/primitives/Shared.h"
#include "runtime/Json.h"
#include "runtime/String.h"

// Json class>>parse: aString
Value primJsonParse(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value source = primitiveArgument(args, 0);
	if (!valueTypeOf(source, VALUE_POINTER)
			|| rawObjectFormat(asObject(source)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	// The parse builds Dictionaries, OrderedCollections and Strings, so the
	// calling method's frame is anchored before any of it.
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	// Through a HANDLE, because the argument is read again inside the parse and
	// the parse allocates: jsonParse promises the result is protected by a
	// handle in the caller's current scope, which is this one.
	String *input = scopeHandle(asObject(primitiveArgument(args, 0)));
	Value parsed;
	_Bool ok = jsonParse(input, &parsed);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return ok ? parsed : PRIMITIVE_FAILED;
}


// Json class>>encode: anObject
Value primJsonEncode(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	String *encoded = NULL;
	// The graph is walked by RAW POINTERS and nothing allocates until the single
	// String at the end (runtime/Json.h), so the argument is read here and stays
	// valid for the whole walk.
	_Bool ok = jsonEncode(primitiveArgument(args, 0), &encoded);
	Value answer = ok ? objectTagged(encoded) : PRIMITIVE_FAILED;
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
