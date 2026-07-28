// Strings.
//
// primIntAsStringBase lives here rather than with the integers because what it
// answers is a String and what it costs is an allocation; the arithmetic file is
// the one domain that allocates nothing at all, and it stays that way.

#include "runtime/primitives/Shared.h"
#include "runtime/String.h"


// A String's hash is over its CONTENTS, and that is what makes it agree with an
// interned Symbol's identity hash by construction (runtime/String.c, asSymbol).
Value primStringHash(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)
			|| rawObjectFormat(asObject(receiver)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	return tagInt(stringHash((RawString *) asObject(receiver)));
}


Value primStringAsSymbol(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)
			|| rawObjectFormat(asObject(receiver)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	// Interning ALLOCATES when the symbol is new, and it can grow the symbol
	// table, so the caller's frames have to be anchored first.
	PRIMITIVE_ALLOCATES(args);
	String *symbol = asSymbol((String *) scopeHandle(asObject(primitiveReceiver(args))));
	Value answer = objectTagged(symbol);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// The digits of an integer, as a String.
//
// It ALLOCATES, so it anchors the calling frame first. The Smalltalk fallback
// beside it in the kernel is the definition of what this must agree with, and it
// stays there: this answers only what it can answer exactly, which is a
// SmallInteger in a base from 2 to 36.
Value primIntAsStringBase(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value baseValue = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_INT) || !valueTypeOf(baseValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t base = asCInt(baseValue);
	if (base < 2 || base > 36) {
		return PRIMITIVE_FAILED; // the fallback signals; guessing here would not
	}

	// Built BACKWARDS into a C buffer first, so the String is allocated once at
	// its exact size. 64 binary digits plus a sign is the widest a SmallInteger
	// can be.
	char digits[72];
	size_t length = 0;
	intptr_t value = asCInt(receiver);
	_Bool negative = value < 0;
	// Through uintptr_t, because negating the most negative value overflows.
	uintptr_t magnitude = negative
		? (uintptr_t) 0 - (uintptr_t) value : (uintptr_t) value;
	if (magnitude == 0) {
		digits[length++] = '0';
	}
	while (magnitude > 0) {
		unsigned digit = (unsigned) (magnitude % (uintptr_t) base);
		digits[length++] = (char) (digit < 10 ? '0' + digit : 'A' + digit - 10);
		magnitude /= (uintptr_t) base;
	}
	if (negative) {
		digits[length++] = '-';
	}

	PRIMITIVE_ALLOCATES(args);
	String *string = newString(length);
	for (size_t i = 0; i < length; i++) {
		string->raw->contents[i] = digits[length - 1 - i];
	}
	Value answer = objectTagged(string);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
