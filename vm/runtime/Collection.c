#include "runtime/Collection.h"
#include "core/Assert.h"
#include "memory/Heap.h"
#include <string.h>

// An OrderedCollection grows by doubling and keeps slack at BOTH ends, so
// prepending is as cheap as appending. The old VM grew by a constant and paid
// O(n^2) on a long append run, measured at 630x on a realistic workload; the
// shape below is the fix, and it is cheap enough that there is no reason to
// wait for a measurement to justify it.
#define ORDCOLL_MIN_CAPACITY 8


Array *newArray(size_t size)
{
	return newObject(&Handles.Array, size);
}


Object *arrayObjectAt(Array *array, size_t index)
{
	Value value = rawArrayAt(array->raw, index);
	return valueTypeOf(value, VALUE_POINTER) ? scopeHandle(asObject(value)) : NULL;
}


void arrayAtPutObject(Array *array, size_t index, Object *object)
{
	ASSERT(index < rawArraySize(array->raw));
	// Through the barrier: an old Array holding a young element is exactly the
	// edge the generational scheme has to know about, and the barrier is the
	// only thing that records it.
	rawObjectStorePtr((RawObject *) array->raw, &array->raw->vars[index],
		object->raw);
}


OrderedCollection *newOrdColl(size_t capacity)
{
	HandleScope scope;
	openHandleScope(&scope);

	if (capacity < ORDCOLL_MIN_CAPACITY) {
		capacity = ORDCOLL_MIN_CAPACITY;
	}
	OrderedCollection *collection = newObject(&Handles.OrderedCollection, 0);
	Array *contents = newArray(capacity);
	// The contents array is allocated AFTER the collection, so this store can
	// see a collection that has already moved; going through the handle is what
	// makes that safe.
	rawObjectStorePtr((RawObject *) collection->raw, &collection->raw->contents,
		(RawObject *) contents->raw);
	// Start in the middle, so the first prepend does not force a grow.
	size_t start = capacity / 4 + 1;
	collection->raw->firstIndex = tagInt((intptr_t) start);
	collection->raw->lastIndex = tagInt((intptr_t) start - 1); // empty
	return closeHandleScope(&scope, collection);
}


size_t ordCollSize(OrderedCollection *collection)
{
	intptr_t first = asCInt(collection->raw->firstIndex);
	intptr_t last = asCInt(collection->raw->lastIndex);
	return last < first ? 0 : (size_t) (last - first + 1);
}


// Move the live span into a bigger array, re-centred. Called only when one end
// runs out, and it re-centres rather than just extending so that a collection
// that alternates prepend and append does not grow on every other operation.
static void ordCollGrow(OrderedCollection *collection)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t size = ordCollSize(collection);
	size_t oldCapacity = rawArraySize((RawArray *) asObject(collection->raw->contents));
	size_t capacity = oldCapacity * 2;
	Array *fresh = newArray(capacity);

	// Re-read the old contents THROUGH the collection handle: allocating
	// `fresh` may have moved it.
	RawArray *from = (RawArray *) asObject(collection->raw->contents);
	intptr_t first = asCInt(collection->raw->firstIndex);
	size_t start = capacity / 4 + 1;
	for (size_t i = 0; i < size; i++) {
		fresh->raw->vars[start - 1 + i] = from->vars[first - 1 + i];
	}
	rawObjectStorePtr((RawObject *) collection->raw, &collection->raw->contents,
		(RawObject *) fresh->raw);
	collection->raw->firstIndex = tagInt((intptr_t) start);
	collection->raw->lastIndex = tagInt((intptr_t) (start + size - 1));
	closeHandleScope(&scope, NULL);
}


// Make room for one more element at the tail. Split out because it is the ONLY
// part of an append that can allocate, and every caller has to know exactly
// where that window is: a caller holding a raw object pointer across it is
// holding a dangling pointer afterwards.
static void ordCollEnsureRoom(OrderedCollection *collection)
{
	RawArray *contents = (RawArray *) asObject(collection->raw->contents);
	intptr_t last = asCInt(collection->raw->lastIndex);
	if ((size_t) last >= rawArraySize(contents)) {
		ordCollGrow(collection);
	}
}


// Append a value that is NOT an object pointer, or one the caller has already
// proved stable. For an object, use ordCollAddObject.
void ordCollAdd(OrderedCollection *collection, Value value)
{
	ordCollEnsureRoom(collection);
	RawArray *contents = (RawArray *) asObject(collection->raw->contents);
	intptr_t last = asCInt(collection->raw->lastIndex);
	if (valueTypeOf(value, VALUE_POINTER)) {
		rawObjectStorePtr((RawObject *) contents, &contents->vars[last],
			asObject(value));
	} else {
		contents->vars[last] = value;
	}
	collection->raw->lastIndex = tagInt(last + 1);
}


void ordCollAddObject(OrderedCollection *collection, Object *object)
{
	// Room FIRST, then read the object's address. Growing allocates, and
	// allocating can move `object`; forming the tagged pointer before the grow
	// would store the address the object used to have. Written the other way
	// round this reads perfectly and is wrong roughly once per collection.
	ordCollEnsureRoom(collection);
	RawArray *contents = (RawArray *) asObject(collection->raw->contents);
	intptr_t last = asCInt(collection->raw->lastIndex);
	rawObjectStorePtr((RawObject *) contents, &contents->vars[last], object->raw);
	collection->raw->lastIndex = tagInt(last + 1);
}


static Value ordCollRawAt(OrderedCollection *collection, size_t index)
{
	RawArray *contents = (RawArray *) asObject(collection->raw->contents);
	intptr_t first = asCInt(collection->raw->firstIndex);
	ASSERT(index < ordCollSize(collection));
	return contents->vars[first - 1 + index];
}


Value ordCollAt(OrderedCollection *collection, size_t index)
{
	return ordCollRawAt(collection, index);
}


Object *ordCollObjectAt(OrderedCollection *collection, size_t index)
{
	Value value = ordCollRawAt(collection, index);
	return valueTypeOf(value, VALUE_POINTER) ? scopeHandle(asObject(value)) : NULL;
}


void ordCollAtPut(OrderedCollection *collection, size_t index, Value value)
{
	ASSERT(index < ordCollSize(collection));
	RawArray *contents = (RawArray *) asObject(collection->raw->contents);
	intptr_t first = asCInt(collection->raw->firstIndex);
	if (valueTypeOf(value, VALUE_POINTER)) {
		rawObjectStorePtr((RawObject *) contents,
			&contents->vars[first - 1 + index], asObject(value));
	} else {
		contents->vars[first - 1 + index] = value;
	}
}


Array *ordCollAsArray(OrderedCollection *collection)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t size = ordCollSize(collection);
	Array *array = newArray(size);
	// Again after the allocation: `collection` may have moved.
	RawArray *contents = (RawArray *) asObject(collection->raw->contents);
	intptr_t first = asCInt(collection->raw->firstIndex);
	for (size_t i = 0; i < size; i++) {
		array->raw->vars[i] = contents->vars[first - 1 + i];
	}
	return closeHandleScope(&scope, array);
}
