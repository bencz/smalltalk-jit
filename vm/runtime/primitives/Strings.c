// Strings.
//
// primIntAsStringBase lives here rather than with the integers because what it
// answers is a String and what it costs is an allocation; the arithmetic file is
// the one domain that allocates nothing at all, and it stays that way.

#include "runtime/primitives/Shared.h"
#include "runtime/String.h"
#include "runtime/Collection.h"
#include <string.h>


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


// ---------------------------------------------------------------------------
// The ASCII fast paths
// ---------------------------------------------------------------------------
//
// WHY THESE ARE PRIMITIVES AT ALL, since every one of them has a working
// Smalltalk body right under its pragma: the fallbacks walk the receiver with
// a SEND PER CHARACTER -- `(self at: i) codePoint` is two sends and a Character
// materialised for each byte -- and these are the HTTP hot path. String.st says
// so at the methods: `sameAsciiCaseAs:` exists precisely so a header name can
// be compared "WITHOUT allocating (unlike `self asLowercase = aString`)".
//
// EXACTLY THE String CLASS, not any byte shape. A Symbol is byte-shaped too and
// must NOT take these paths: `asLowercase` on a Symbol has to answer a String
// (the .st fallback goes through `self species`, and a Symbol is immutable), and
// `copyFrom:to:` on one is the same story. So the guard is the class, and
// anything else falls through to the Smalltalk body that already handles it.
//
// ASCII ONLY, which is what the names say. Nothing here looks at a locale and
// nothing here decodes UTF-8; a byte outside A-Z / a-z is copied through.

// The receiver as a plain String, or NULL for anything else -- a Symbol, a
// ByteArray, an immediate.
static RawString *plainString(Value value)
{
	if (!valueTypeOf(value, VALUE_POINTER) || Handles.String.raw == NULL) {
		return NULL;
	}
	RawObject *object = asObject(value);
	return rawObjectClassIndex(object) == classIndexOf(&Handles.String)
		? (RawString *) object : NULL;
}


// Any byte-shaped operand, for the COMPARISONS: those only read, so a Symbol
// on either side is fine and answering false for it would be wrong.
static _Bool byteOperand(Value value, const uint8_t **bytes, size_t *size)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return 0;
	}
	*bytes = rawObjectBytes(object);
	*size = rawObjectElementCount(object);
	return 1;
}


static _Bool asciiCaseEqualBytes(const uint8_t *a, const uint8_t *b, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (a[i] == b[i]) {
			continue;
		}
		// The fold the kernel's own fallback uses: OR bit 5 and require the
		// result to be a letter. Doing it the other way round -- folding first
		// and comparing -- would make '[' equal '{', because they differ in
		// exactly that bit and are not letters.
		uint8_t folded = (uint8_t) (a[i] | 32);
		if (folded < 'a' || folded > 'z' || folded != (uint8_t) (b[i] | 32)) {
			return 0;
		}
	}
	return 1;
}


// String>>sameAsciiCaseAs: aString
Value primStringAsciiCaseEquals(Value *args, uint64_t argc)
{
	const uint8_t *a, *b;
	size_t sizeA, sizeB;
	if (argc != 1 || !byteOperand(primitiveReceiver(args), &a, &sizeA)
			|| !byteOperand(primitiveArgument(args, 0), &b, &sizeB)) {
		return PRIMITIVE_FAILED;
	}
	if (sizeA != sizeB) {
		return booleanResult(0);
	}
	return booleanResult(asciiCaseEqualBytes(a, b, sizeA));
}


// String>>startsWithAsciiCase: aPrefix
Value primStringStartsWithAsciiCase(Value *args, uint64_t argc)
{
	const uint8_t *a, *b;
	size_t sizeA, sizeB;
	if (argc != 1 || !byteOperand(primitiveReceiver(args), &a, &sizeA)
			|| !byteOperand(primitiveArgument(args, 0), &b, &sizeB)) {
		return PRIMITIVE_FAILED;
	}
	if (sizeB > sizeA) {
		return booleanResult(0);
	}
	return booleanResult(asciiCaseEqualBytes(a, b, sizeB));
}


// String>>= aCollection
//
// A memcmp, and a FAILURE for anything that is not byte-shaped so the kernel's
// `^super =` walks the operand element by element.
//
// THE CLASSES MUST MATCH, and that is not an extra check, it is the FIRST LINE
// of the method this replaces: SequenceableCollection>>= opens with
// `self class == aCollection class ifFalse: [^false]`. Comparing bytes alone
// made `'hello' = #hello` answer TRUE, which tests/SymbolTest.st pins in both
// directions -- a Symbol is interned so that identity IS its equality, and a
// String that equalled one would put the two in the same Dictionary bucket
// with different lookup rules.
Value primArrayEquals(Value *args, uint64_t argc)
{
	const uint8_t *a, *b;
	size_t sizeA, sizeB;
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (argc != 1 || !byteOperand(receiver, &a, &sizeA)
			|| !byteOperand(argument, &b, &sizeB)) {
		return PRIMITIVE_FAILED;
	}
	if (rawObjectClassIndex(asObject(receiver))
			!= rawObjectClassIndex(asObject(argument))) {
		return booleanResult(0);
	}
	return booleanResult(sizeA == sizeB && memcmp(a, b, sizeA) == 0);
}


// String>>indexOfByte: anInteger startingAt: start
//
// A 1-based index or 0, which is what the HTTP parser reads to find a line
// terminator. `start` past the end is not an error, it is 0: a scan that found
// nothing and a scan that had nowhere to look answer the same thing.
Value primIndexOfByte(Value *args, uint64_t argc)
{
	const uint8_t *bytes;
	size_t size;
	Value target = primitiveArgument(args, 0);
	Value start = primitiveArgument(args, 1);
	if (argc != 2 || !byteOperand(primitiveReceiver(args), &bytes, &size)
			|| !valueTypeOf(target, VALUE_INT) || !valueTypeOf(start, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t wanted = asCInt(target);
	intptr_t from = asCInt(start);
	if (wanted < 0 || wanted > 255 || from < 1) {
		return PRIMITIVE_FAILED;
	}
	if ((size_t) from > size) {
		return tagInt(0);
	}
	const uint8_t *found = memchr(bytes + (from - 1), (int) wanted,
		size - (size_t) (from - 1));
	return tagInt(found == NULL ? 0 : (intptr_t) (found - bytes) + 1);
}


// String>>asLowercase and String>>asUppercase.
//
// ONE implementation, because the two differ by a constant and a range, and a
// second copy of a byte loop is a second place for the range to be wrong.
static Value asciiFold(Value *args, uint64_t argc, _Bool toUpper)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	RawString *receiver = plainString(primitiveReceiver(args));
	if (receiver == NULL) {
		return PRIMITIVE_FAILED; // a Symbol: the fallback answers a String
	}
	size_t size = rawObjectElementCount((RawObject *) receiver);
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	String *result = newString(size);
	// RE-READ THE SOURCE after allocating: newString can collect, and the
	// receiver is a heap object the collector is entitled to have moved.
	const uint8_t *source = rawObjectBytes(asObject(primitiveReceiver(args)));
	uint8_t *destination = (uint8_t *) result->raw->contents;
	for (size_t i = 0; i < size; i++) {
		uint8_t c = source[i];
		if (toUpper) {
			destination[i] = (c >= 'a' && c <= 'z') ? (uint8_t) (c - 32) : c;
		} else {
			destination[i] = (c >= 'A' && c <= 'Z') ? (uint8_t) (c + 32) : c;
		}
	}
	Value answer = objectTagged(result);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primStringAsciiLowercase(Value *args, uint64_t argc)
{
	return asciiFold(args, argc, 0);
}


Value primStringAsciiUppercase(Value *args, uint64_t argc)
{
	return asciiFold(args, argc, 1);
}


// The kernel's separator set, and it is the kernel's: `Character>>isSeparator`
// answers true for tab, lf, ff, cr and space, and trimSeparators' own comment
// lists exactly those five. A sixth here would be a second opinion.
static _Bool isSeparatorByte(uint8_t c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}


// String>>trimSeparators
Value primStringTrimSeparators(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	RawString *receiver = plainString(primitiveReceiver(args));
	if (receiver == NULL) {
		return PRIMITIVE_FAILED;
	}
	const uint8_t *bytes = rawObjectBytes((RawObject *) receiver);
	size_t size = rawObjectElementCount((RawObject *) receiver);
	size_t start = 0;
	while (start < size && isSeparatorByte(bytes[start])) {
		start++;
	}
	size_t stop = size;
	while (stop > start && isSeparatorByte(bytes[stop - 1])) {
		stop--;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	String *result = newString(stop - start);
	// Re-read after the allocation, for the same reason as the fold above.
	memcpy(result->raw->contents,
		rawObjectBytes(asObject(primitiveReceiver(args))) + start, stop - start);
	Value answer = objectTagged(result);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// SequenceableCollection>>copyFrom: start to: stop
//
// PLAIN STRINGS ONLY. Every other species -- Symbol, Array, OrderedCollection --
// falls through to the generic `self species new: ... replaceFrom:` path, which
// is what the kernel's comment says and what keeps a Symbol's copy a String.
//
// An EMPTY range is legal and is what `copyFrom: 1 to: 0` means: the kernel
// writes it deliberately, so stop == start - 1 answers '' rather than failing.
Value primStringCopyFromTo(Value *args, uint64_t argc)
{
	Value startValue = primitiveArgument(args, 0);
	Value stopValue = primitiveArgument(args, 1);
	if (argc != 2 || !valueTypeOf(startValue, VALUE_INT)
			|| !valueTypeOf(stopValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawString *receiver = plainString(primitiveReceiver(args));
	if (receiver == NULL) {
		return PRIMITIVE_FAILED;
	}
	size_t size = rawObjectElementCount((RawObject *) receiver);
	intptr_t start = asCInt(startValue);
	intptr_t stop = asCInt(stopValue);
	if (start < 1 || stop < start - 1 || (size_t) stop > size) {
		return PRIMITIVE_FAILED; // the fallback raises with the range
	}
	size_t length = (size_t) (stop - start + 1);
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	String *result = newString(length);
	memcpy(result->raw->contents,
		rawObjectBytes(asObject(primitiveReceiver(args))) + (start - 1), length);
	Value answer = objectTagged(result);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// SequenceableCollection>>splitBy: anObject
//
// A plain String split on a CHARACTER, scanning and building the collection in
// one primitive. An Array or Symbol receiver, or a non-Character delimiter,
// falls through to the generic loop.
//
// EMPTY RUNS ARE DROPPED, which is what the fallback does: it appends a piece
// only when `last = i` is false, so 'a,,b' splits into two pieces and not
// three. That is a semantic, not an accident, and it is the reason this reads
// the fallback rather than doing the obvious thing.
Value primStringSplitBy(Value *args, uint64_t argc)
{
	Value separator = primitiveArgument(args, 0);
	if (argc != 1 || !valueTypeOf(separator, VALUE_CHAR)) {
		return PRIMITIVE_FAILED;
	}
	if (plainString(primitiveReceiver(args)) == NULL) {
		return PRIMITIVE_FAILED;
	}
	uint8_t wanted = (uint8_t) asCChar(separator);
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *pieces = newOrdColl(8);
	// EVERY read of the receiver's bytes happens after the allocation that
	// precedes it, because each piece allocates and any of them can collect.
	size_t size = rawObjectElementCount(asObject(primitiveReceiver(args)));
	size_t last = 0;
	for (size_t i = 0; i <= size; i++) {
		_Bool atSeparator = i < size
			&& rawObjectBytes(asObject(primitiveReceiver(args)))[i] == wanted;
		if (!atSeparator && i < size) {
			continue;
		}
		if (i > last) {
			String *piece = newString(i - last);
			memcpy(piece->raw->contents,
				rawObjectBytes(asObject(primitiveReceiver(args))) + last, i - last);
			ordCollAddObject(pieces, (Object *) piece);
		}
		last = i + 1;
	}
	Value answer = objectTagged(pieces);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// String>>asNumber
//
// A PLAIN BASE-10 SmallInteger AND NOTHING ELSE, which is what the kernel's
// comment promises: a sign, a float, an exponent, a value past SmallInteger or
// any trailing text all fall through to `Number readFrom:`, the full stream
// parser. Answering a partial parse here would be a wrong answer, not a slow
// one -- '12abc' is not 12.
Value primStringToInteger(Value *args, uint64_t argc)
{
	const uint8_t *bytes;
	size_t size;
	if (argc != 0 || !byteOperand(primitiveReceiver(args), &bytes, &size)
			|| size == 0) {
		return PRIMITIVE_FAILED;
	}
	uintptr_t value = 0;
	for (size_t i = 0; i < size; i++) {
		if (bytes[i] < '0' || bytes[i] > '9') {
			return PRIMITIVE_FAILED;
		}
		// Overflow BEFORE it happens, against the SmallInteger ceiling and not
		// against the C type: a sum that fits an intptr_t may still not fit the
		// 62-bit signed payload, which is the trap Shared.h names.
		if (value > ((uintptr_t) SMALL_INT_MAX - (uintptr_t) (bytes[i] - '0')) / 10) {
			return PRIMITIVE_FAILED;
		}
		value = value * 10 + (uintptr_t) (bytes[i] - '0');
	}
	return tagInt((intptr_t) value);
}
