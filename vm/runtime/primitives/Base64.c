// Base 64, over the C runtime in runtime/Base64.c.
//
// THE RUNTIME WAS ALREADY HERE, the third time this pattern has come up in this
// campaign after BigInt and Json: RFC 4648, standard alphabet, '=' padding, and
// a decoder that VALIDATES BEFORE ALLOCATING. What was missing were the three
// primitives, so every base-64 operation ran the Smalltalk reference
// implementation -- which is a send per character with a Character materialised
// for each byte.
//
// THE .st IS THE SPECIFICATION AND STAYS SO. Base64.st says it in as many
// words: "the Smalltalk bodies below the pragmas are the validating fallback
// and the reference implementation - keep semantics in sync." So a malformed
// input FAILS here rather than being diagnosed here: the fallback re-scans and
// raises the precise InvalidArgumentError, and there is one place that decides
// what "malformed" means.
//
// THE base64url VARIANT NEEDS NOTHING. encodeUrlSafe:/decodeUrlSafe: are
// written in the .st on top of these two -- translate the alphabet, strip or
// restore the padding -- so they get the fast path for free and no fourth
// primitive exists to disagree with them.

#include "runtime/primitives/Shared.h"
#include "runtime/Base64.h"
#include "runtime/String.h"

// The receiver of the class-side methods is Base64 itself and is not read; what
// matters is the argument, and its EXACT class, because the C layer indexes the
// bytes directly. A String and a ByteArray are both acceptable input to encode:
// (`bytesOf:` in the kernel accepts either), and both are byte-shaped, which is
// the only thing the encoder needs.
static _Bool base64Bytes(Value value, Object **object)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *raw = asObject(value);
	if (rawObjectFormat(raw) != FORMAT_BYTES) {
		return 0;
	}
	*object = scopeHandle(raw);
	return 1;
}


// Base64 class>>encode: aCollection
Value primBase64Encode(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	// The handle is taken BEFORE the allocation inside base64Encode, which is
	// what its header promises it needs: the input is a heap object and the
	// output String is allocated in the middle of reading it.
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Object *input;
	String *encoded = NULL;
	Value answer = PRIMITIVE_FAILED;
	if (base64Bytes(primitiveArgument(args, 0), &input)
			&& base64Encode(input, &encoded)) {
		answer = objectTagged(encoded);
	}
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Base64 class>>decode: aString  and  Base64 class>>decodeBytes: aString
//
// ONE implementation with the output class as the difference, which is exactly
// what the two kernel methods are: `decodeBytes:` is documented as "Like
// decode: but answers a ByteArray".
static Value base64DecodeInto(Value *args, Class *outputClass)
{
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Object *input;
	Object *decoded = NULL;
	Value answer = PRIMITIVE_FAILED;
	if (base64Bytes(primitiveArgument(args, 0), &input)
			&& base64Decode((String *) input, outputClass, &decoded)) {
		answer = objectTagged(decoded);
	}
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primBase64Decode(Value *args, uint64_t argc)
{
	if (argc != 1 || Handles.String.raw == NULL) {
		return PRIMITIVE_FAILED;
	}
	return base64DecodeInto(args, &Handles.String);
}


Value primBase64DecodeBytes(Value *args, uint64_t argc)
{
	if (argc != 1 || Handles.ByteArray.raw == NULL) {
		return PRIMITIVE_FAILED;
	}
	return base64DecodeInto(args, &Handles.ByteArray);
}
