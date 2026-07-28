#ifndef STRING_H
#define STRING_H

// Strings and Symbols. Both are FORMAT_BYTES: one body word holding the byte
// count, then the bytes. The layout falls straight out of the object header
// (core/Object.h) rather than being a special case, which is why the collector
// never has to know what a String is.
//
// A Symbol is a String that has been INTERNED: one object per distinct
// character sequence in a heap, so symbol identity is pointer identity and a
// selector comparison is a single compare. Interning is the only reason the
// symbol table exists.

#include "core/Handle.h"
#include "core/Object.h"
#include <stdio.h>

typedef struct {
	OBJECT_HEADER;
	uint64_t size;     // FORMAT_BYTES puts the count in body word 0
	char contents[];
} RawString;
OBJECT_HANDLE(String);

String *newString(size_t size);
String *stringFromC(const char *buffer);
String *stringFromBytes(const char *bytes, size_t size);

// Intern: answer the ONE Symbol in this heap with these characters.
String *asSymbol(String *string);
String *symbolFromC(const char *buffer);

uint32_t stringHash(RawString *string);
_Bool stringEquals(String *a, String *b);
_Bool stringEqualsC(String *a, const char *b);
_Bool rawStringEqualsBytes(RawString *a, const char *bytes, size_t size);
// Number of arguments a selector takes, from its shape: unary is 0, a binary
// operator is 1, a keyword selector is one per colon.
size_t selectorArgumentCount(RawString *selector);

void printRawString(RawString *string);
// The same, on a chosen stream. It exists because a message printed on the way
// to abort() has to go to STDERR: abort does not flush stdio, so a diagnostic
// written to a redirected (block-buffered) stdout dies with the process. That is
// how the old VM's code-ceiling abort shipped silent, and how the v2's
// doesNotUnderstand message shipped with an empty selector.
void fprintRawString(FILE *out, RawString *string);
void printValue(Value value);

// Copy the characters into `buffer` and NUL-terminate. A Smalltalk String is
// counted and not terminated, so anything handing one to a C interface (the
// tokenizer takes a `char *`) has to go through this.
void stringPrintOn(String *string, char *buffer);


static inline size_t rawStringSize(RawString *string)
{
	return (size_t) string->size;
}

#endif
