#ifndef COLLECTION_H
#define COLLECTION_H

// Array and OrderedCollection.
//
// Array is FORMAT_INDEXED_POINTERS: one body word holding the element count,
// then the elements. Nothing about it is special-cased in the collector, which
// strides on the header like it does for everything else.
//
// OrderedCollection is FORMAT_POINTERS over three slots. Two of them hold
// tagged SmallIntegers rather than pointers, which is fine and is exactly why
// the collector tests the TAG of every slot it visits instead of trusting the
// format to mean "all of these are pointers".

#include "core/Handle.h"
#include "core/Object.h"

typedef struct {
	OBJECT_HEADER;
	Value contents;   // an Array, with slack at both ends
	Value firstIndex; // tagged SmallInteger, 1-based, inclusive
	Value lastIndex;  // tagged SmallInteger, inclusive; empty when < firstIndex
} RawOrderedCollection;
OBJECT_HANDLE(OrderedCollection);

Array *newArray(size_t size);
Object *arrayObjectAt(Array *array, size_t index);
void arrayAtPutObject(Array *array, size_t index, Object *object);

OrderedCollection *newOrdColl(size_t capacity);
size_t ordCollSize(OrderedCollection *collection);
void ordCollAdd(OrderedCollection *collection, Value value);
void ordCollAddObject(OrderedCollection *collection, Object *object);
Value ordCollAt(OrderedCollection *collection, size_t index);
Object *ordCollObjectAt(OrderedCollection *collection, size_t index);
void ordCollAtPut(OrderedCollection *collection, size_t index, Value value);
Array *ordCollAsArray(OrderedCollection *collection);


static inline size_t rawArraySize(RawArray *array)
{
	return (size_t) array->size;
}


static inline Value rawArrayAt(RawArray *array, size_t index)
{
	ASSERT(index < rawArraySize(array));
	return array->vars[index];
}

#endif
