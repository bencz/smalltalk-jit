#ifndef DICTIONARY_H
#define DICTIONARY_H

// Open-addressed dictionary: `contents` is an Array whose slots hold
// Associations, `tally` counts the live ones. Linear probing, power-of-two
// capacity, grown at three quarters full.
//
// Two flavours, and the difference is only which comparison and which hash:
//
//   SYMBOL   keys are interned Symbols, so the comparison is IDENTITY and the
//            hash is the symbol's header hash. This is what a method dictionary
//            is, and it is the lookup every send performs.
//   STRING   keys are Strings compared by CONTENT, for the places that have not
//            interned yet (the parser's tables, source-level lookups).

#include "core/Handle.h"
#include "core/Object.h"
#include "runtime/Collection.h"
#include "runtime/String.h"

typedef struct {
	OBJECT_HEADER;
	Value contents; // Array of Association, nil in empty slots
	Value tally;    // tagged SmallInteger: live associations
} RawDictionary;
OBJECT_HANDLE(Dictionary);

Dictionary *newDictionary(size_t capacity);
size_t dictSize(Dictionary *dictionary);

// Symbol keys: identity comparison on interned Symbols.
Association *symbolDictAtPut(Dictionary *dictionary, String *key, Value value);
Association *symbolDictAtPutObject(Dictionary *dictionary, String *key, Object *value);
Value symbolDictAt(Dictionary *dictionary, String *key);
Association *symbolDictAssocAt(Dictionary *dictionary, String *key);

// String keys: content comparison, for keys not yet interned.
Association *stringDictAtPut(Dictionary *dictionary, String *key, Value value);
Value stringDictAt(Dictionary *dictionary, String *key);
Association *stringDictAssocAt(Dictionary *dictionary, String *key);

#endif
