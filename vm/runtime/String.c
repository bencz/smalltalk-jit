#include "runtime/String.h"
#include "core/Assert.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"
#include <stdio.h>
#include <string.h>


String *newString(size_t size)
{
	return newObject(&Handles.String, size);
}


String *stringFromBytes(const char *bytes, size_t size)
{
	String *string = newString(size);
	memcpy(string->raw->contents, bytes, size);
	return string;
}


String *stringFromC(const char *buffer)
{
	return stringFromBytes(buffer, strlen(buffer));
}


// FNV-1a over the bytes. Any decent byte hash would do; what matters is that it
// is computed from CONTENTS and not from identity, because two Strings with the
// same characters have to reach the same symbol-table bucket.
uint32_t stringHash(RawString *string)
{
	uint32_t hash = 2166136261u;
	size_t size = rawStringSize(string);
	const char *bytes = string->contents;
	for (size_t i = 0; i < size; i++) {
		hash ^= (uint8_t) bytes[i];
		hash *= 16777619u;
	}
	// Folded into the 22 bits an object header can carry, so that an interned
	// Symbol's IDENTITY hash and its CONTENT hash are the same number. That is
	// what lets an identity dictionary and a string dictionary agree on where a
	// symbol lives, instead of needing two hashes per symbol.
	return (hash ^ (hash >> 22)) & (uint32_t) OBJ_HASH_MASK;
}


_Bool rawStringEqualsBytes(RawString *a, const char *bytes, size_t size)
{
	return rawStringSize(a) == size && memcmp(a->contents, bytes, size) == 0;
}


_Bool stringEquals(String *a, String *b)
{
	// Interned symbols short-circuit: identity IS equality for them, which is
	// the whole point of interning.
	if (a->raw == b->raw) {
		return 1;
	}
	return rawStringEqualsBytes(a->raw, b->raw->contents, rawStringSize(b->raw));
}


_Bool stringEqualsC(String *a, const char *b)
{
	return rawStringEqualsBytes(a->raw, b, strlen(b));
}


size_t selectorArgumentCount(RawString *selector)
{
	size_t size = rawStringSize(selector);
	if (size == 0) {
		return 0;
	}
	const char *bytes = selector->contents;
	size_t colons = 0;
	for (size_t i = 0; i < size; i++) {
		if (bytes[i] == ':') {
			colons++;
		}
	}
	if (colons > 0) {
		return colons; // keyword: one argument per colon
	}
	// Otherwise a binary operator takes one and a unary selector takes none.
	// The FIRST byte decides, because an identifier can never begin with an
	// operator character.
	char first = bytes[0];
	_Bool alphabetic = (first >= 'a' && first <= 'z')
		|| (first >= 'A' && first <= 'Z') || first == '_';
	return alphabetic ? 0 : 1;
}


// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------
//
// One open-addressed table per heap, holding Symbols directly. A Symbol is a
// String whose class index is Symbol's and which lives in that table; nothing
// else distinguishes it. Interning is what makes selector comparison a single
// pointer compare, which is the operation every send performs.

static void growSymbolTable(void);


static _Bool symbolTableInsert(RawArray *table, RawObject *symbol, uint32_t hash)
{
	size_t capacity = (size_t) table->size;
	size_t index = hash & (capacity - 1);
	for (size_t probe = 0; probe < capacity; probe++) {
		Value slot = table->vars[index];
		if (!valueTypeOf(slot, VALUE_POINTER)) {
			// THROUGH THE BARRIER. The symbol table is 8 KB on its first
			// allocation, which sends it straight to the non-moving old space,
			// while every symbol it holds is born young. An old-to-young edge
			// that the barrier never recorded is invisible to a young
			// collection: the symbols are not kept alive, the entries dangle,
			// and interning silently starts answering fresh objects for
			// selectors that already exist. Which is exactly what the
			// self-test caught.
			rawObjectStorePtr((RawObject *) table, &table->vars[index], symbol);
			return 1;
		}
		index = (index + 1) & (capacity - 1);
	}
	return 0;
}


String *asSymbol(String *string)
{
	Heap *heap = CurrentThread.heap;
	heapSymbolLockEnter(heap);

	RawArray *table = Handles.symbolTable.raw;
	size_t capacity = (size_t) table->size;
	uint32_t hash = stringHash(string->raw);
	size_t index = hash & (capacity - 1);

	for (size_t probe = 0; probe < capacity; probe++) {
		Value slot = table->vars[index];
		if (!valueTypeOf(slot, VALUE_POINTER)) {
			break; // empty slot: not present
		}
		RawString *candidate = (RawString *) asObject(slot);
		if (rawStringEqualsBytes(candidate, string->raw->contents,
				rawStringSize(string->raw))) {
			heapSymbolLockLeave(heap);
			return scopeHandle(candidate);
		}
		index = (index + 1) & (capacity - 1);
	}

	// Not present. Copy the characters into a fresh Symbol: a COPY and not the
	// argument, because the caller's String is mutable and a Symbol must not be.
	size_t size = rawStringSize(string->raw);
	String *symbol = newObject(&Handles.Symbol, size);
	memcpy(symbol->raw->contents, string->raw->contents, size);
	symbol->raw->header = (symbol->raw->header & ~(OBJ_HASH_MASK << OBJ_HASH_SHIFT))
		| ((uint64_t) hash << OBJ_HASH_SHIFT);

	// The allocation may have collected, which MOVES the table. Re-read it
	// through the handle rather than reusing the pointer from above.
	if (heap->symbolCount * 4 >= (size_t) (Handles.symbolTable.raw)->size * 3) {
		growSymbolTable();
	}
	table = Handles.symbolTable.raw;
	_Bool inserted = symbolTableInsert(table, (RawObject *) symbol->raw, hash);
	ASSERT(inserted);
	heap->symbolCount++;

	heapSymbolLockLeave(heap);
	return symbol;
}


String *symbolFromC(const char *buffer)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *symbol = asSymbol(stringFromC(buffer));
	return closeHandleScope(&scope, symbol);
}


static void growSymbolTable(void)
{
	HandleScope scope;
	openHandleScope(&scope);

	Array *old = &Handles.symbolTable;
	size_t oldCapacity = (size_t) old->raw->size;
	Array *fresh = newArray(oldCapacity * 2);

	// `old` is read through its handle here: allocating `fresh` may have moved
	// it, and this is exactly the window where a raw pointer captured before
	// the allocation would dangle.
	RawArray *from = old->raw;
	RawArray *to = fresh->raw;
	for (size_t i = 0; i < oldCapacity; i++) {
		Value slot = from->vars[i];
		if (!valueTypeOf(slot, VALUE_POINTER)) {
			continue;
		}
		RawObject *symbol = asObject(slot);
		symbolTableInsert(to, symbol, rawObjectHash(symbol));
	}
	// symbolTableInsert goes through the barrier, so `to` is already recorded
	// if it needed to be.
	RawArray *published = fresh->raw;
	closeHandleScope(&scope, NULL);
	// Handles.symbolTable is itself a root, so it is written AFTER the scope
	// closes and holds the raw pointer directly.
	Handles.symbolTable.raw = published;
}


void printRawString(RawString *string)
{
	printf("%.*s", (int) rawStringSize(string), string->contents);
}


void printValue(Value value)
{
	switch (value & 3) {
	case VALUE_INT:
		printf("%ld", (long) asCInt(value));
		break;
	case VALUE_CHAR:
		printf("$%c", asCChar(value));
		break;
	case VALUE_FLOAT:
		printf("%g", floatValueOf(value));
		break;
	default:
		printf("<object %p>", (void *) asObject(value));
		break;
	}
}


void stringPrintOn(String *string, char *buffer)
{
	size_t size = rawStringSize(string->raw);
	memcpy(buffer, string->raw->contents, size);
	buffer[size] = '\0';
}
