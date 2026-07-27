#include "runtime/Dictionary.h"
#include "core/Assert.h"
#include "memory/Heap.h"

#define DICT_MIN_CAPACITY 8


static size_t roundUpPowerOfTwo(size_t n)
{
	size_t capacity = DICT_MIN_CAPACITY;
	while (capacity < n) {
		capacity *= 2;
	}
	return capacity;
}


Dictionary *newDictionary(size_t capacity)
{
	HandleScope scope;
	openHandleScope(&scope);

	Dictionary *dictionary = newObject(&Handles.Dictionary, 0);
	Array *contents = newArray(roundUpPowerOfTwo(capacity));
	rawObjectStorePtr((RawObject *) dictionary->raw, &dictionary->raw->contents,
		(RawObject *) contents->raw);
	dictionary->raw->tally = tagInt(0);
	return closeHandleScope(&scope, dictionary);
}


size_t dictSize(Dictionary *dictionary)
{
	return (size_t) asCInt(dictionary->raw->tally);
}


// How a key is compared and hashed. The two flavours differ ONLY here, so the
// probe loop below exists once.
typedef _Bool (*KeyMatch)(RawObject *storedKey, RawString *key);


static _Bool matchByIdentity(RawObject *storedKey, RawString *key)
{
	return storedKey == (RawObject *) key;
}


static _Bool matchByContent(RawObject *storedKey, RawString *key)
{
	return rawStringEqualsBytes((RawString *) storedKey, key->contents,
		rawStringSize(key));
}


// Find the slot a key belongs in: either the association already there, or the
// first empty slot on its probe chain. Returns the index; `*found` says which.
static size_t probe(RawArray *contents, RawString *key, uint32_t hash,
	KeyMatch match, _Bool *found)
{
	size_t capacity = rawArraySize(contents);
	size_t index = hash & (capacity - 1);
	for (size_t step = 0; step < capacity; step++) {
		Value slot = contents->vars[index];
		if (!valueTypeOf(slot, VALUE_POINTER)) {
			*found = 0;
			return index;
		}
		RawAssociation *association = (RawAssociation *) asObject(slot);
		if (match(asObject(association->key), key)) {
			*found = 1;
			return index;
		}
		index = (index + 1) & (capacity - 1);
	}
	FAIL(); // a dictionary kept below three quarters full always has a hole
}


static uint32_t keyHash(RawString *key, _Bool byIdentity)
{
	// For an interned Symbol the two are the SAME number by construction (see
	// asSymbol), so this only matters for un-interned Strings.
	return byIdentity ? rawObjectHash((RawObject *) key) : stringHash(key);
}


static void dictGrow(Dictionary *dictionary, _Bool byIdentity)
{
	HandleScope scope;
	openHandleScope(&scope);

	RawArray *old = (RawArray *) asObject(dictionary->raw->contents);
	size_t oldCapacity = rawArraySize(old);
	Array *fresh = newArray(oldCapacity * 2);

	// Re-read after the allocation: `dictionary` may have moved.
	old = (RawArray *) asObject(dictionary->raw->contents);
	KeyMatch match = byIdentity ? matchByIdentity : matchByContent;
	for (size_t i = 0; i < oldCapacity; i++) {
		Value slot = old->vars[i];
		if (!valueTypeOf(slot, VALUE_POINTER)) {
			continue;
		}
		RawAssociation *association = (RawAssociation *) asObject(slot);
		RawString *key = (RawString *) asObject(association->key);
		_Bool found;
		size_t index = probe(fresh->raw, key, keyHash(key, byIdentity), match, &found);
		ASSERT(!found);
		rawObjectStorePtr((RawObject *) fresh->raw, &fresh->raw->vars[index],
			(RawObject *) association);
	}
	rawObjectStorePtr((RawObject *) dictionary->raw, &dictionary->raw->contents,
		(RawObject *) fresh->raw);
	closeHandleScope(&scope, NULL);
}


static Association *dictAtPut(Dictionary *dictionary, String *key, Value value,
	Object *valueObject, _Bool byIdentity)
{
	HandleScope scope;
	openHandleScope(&scope);

	KeyMatch match = byIdentity ? matchByIdentity : matchByContent;
	uint32_t hash = keyHash(key->raw, byIdentity);

	_Bool found;
	size_t index = probe((RawArray *) asObject(dictionary->raw->contents),
		key->raw, hash, match, &found);
	if (found) {
		RawArray *contents = (RawArray *) asObject(dictionary->raw->contents);
		RawAssociation *association =
			(RawAssociation *) asObject(contents->vars[index]);
		Value stored = valueObject != NULL ? tagPtr(valueObject->raw) : value;
		if (valueTypeOf(stored, VALUE_POINTER)) {
			rawObjectStorePtr((RawObject *) association, &association->value,
				asObject(stored));
		} else {
			association->value = stored;
		}
		return closeHandleScope(&scope, scopeHandle(association));
	}

	// Grow BEFORE building the association, so the probe index computed above
	// is not invalidated by a rehash between here and the store.
	size_t tally = dictSize(dictionary) + 1;
	if (tally * 4 >= rawArraySize((RawArray *) asObject(dictionary->raw->contents)) * 3) {
		dictGrow(dictionary, byIdentity);
	}

	Association *association = newObject(&Handles.Association, 0);
	rawObjectStorePtr((RawObject *) association->raw, &association->raw->key,
		(RawObject *) key->raw);
	Value stored = valueObject != NULL ? tagPtr(valueObject->raw) : value;
	if (valueTypeOf(stored, VALUE_POINTER)) {
		rawObjectStorePtr((RawObject *) association->raw, &association->raw->value,
			asObject(stored));
	} else {
		association->raw->value = stored;
	}

	// Re-probe: the grow above rehashed, and allocating the association may
	// have collected. Both invalidate an index computed earlier.
	index = probe((RawArray *) asObject(dictionary->raw->contents),
		key->raw, hash, match, &found);
	ASSERT(!found);
	RawArray *contents = (RawArray *) asObject(dictionary->raw->contents);
	rawObjectStorePtr((RawObject *) contents, &contents->vars[index],
		(RawObject *) association->raw);
	dictionary->raw->tally = tagInt((intptr_t) tally);
	return closeHandleScope(&scope, association);
}


static Association *dictAssocAt(Dictionary *dictionary, String *key, _Bool byIdentity)
{
	KeyMatch match = byIdentity ? matchByIdentity : matchByContent;
	_Bool found;
	RawArray *contents = (RawArray *) asObject(dictionary->raw->contents);
	size_t index = probe(contents, key->raw, keyHash(key->raw, byIdentity),
		match, &found);
	if (!found) {
		return NULL;
	}
	return scopeHandle(asObject(contents->vars[index]));
}


Association *symbolDictAtPut(Dictionary *dictionary, String *key, Value value)
{
	return dictAtPut(dictionary, key, value, NULL, 1);
}


Association *symbolDictAtPutObject(Dictionary *dictionary, String *key, Object *value)
{
	return dictAtPut(dictionary, key, 0, value, 1);
}


Association *symbolDictAssocAt(Dictionary *dictionary, String *key)
{
	return dictAssocAt(dictionary, key, 1);
}


Value symbolDictAt(Dictionary *dictionary, String *key)
{
	Association *association = dictAssocAt(dictionary, key, 1);
	return association == NULL ? 0 : association->raw->value;
}


Association *stringDictAtPut(Dictionary *dictionary, String *key, Value value)
{
	return dictAtPut(dictionary, key, value, NULL, 0);
}


Association *stringDictAssocAt(Dictionary *dictionary, String *key)
{
	return dictAssocAt(dictionary, key, 0);
}


Value stringDictAt(Dictionary *dictionary, String *key)
{
	Association *association = dictAssocAt(dictionary, key, 0);
	return association == NULL ? 0 : association->raw->value;
}
