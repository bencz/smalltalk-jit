// Characters.
//
// A Character is an IMMEDIATE: the code point IS the payload, so both of these
// are a tag operation and neither touches the heap.

#include "runtime/primitives/Shared.h"


Value primCharacterCode(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_CHAR)) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) (unsigned char) asCChar(receiver));
}


Value primCharacterNew(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value code = primitiveArgument(args, 0);
	if (!valueTypeOf(code, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t point = asCInt(code);
	// Out of range FAILS rather than truncating: the Smalltalk fallback is what
	// knows how to signal, and a truncated code point is a wrong character.
	if (point < 0 || point > 255) {
		return PRIMITIVE_FAILED;
	}
	return tagChar((char) point);
}
