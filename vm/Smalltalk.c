#include "Smalltalk.h"
#include "Thread.h"
#include "Heap.h"
#include "Handle.h"
#include "StackFrame.h"
#include "CodeDescriptors.h"
#include "Thread.h"
#include "Assert.h"
#include <string.h>
#include <stdio.h>

static void swapObjectPointers(Object *object, Object *other);
static void swapObjectInNewSpace(Object *old, Object *new);
static void swapObjectInOldSpace(Object *object, Object *other);
static void swapObjectOnStack(Object *object, Object *other);
static void iterateObject(RawObject *object, Object *old, Object *new);


// The symbol table is an open-addressed (linear-probe) hash array with a
// power-of-two size. A full table would make the probe loop below spin forever,
// so we track occupancy and grow (rehash into a 2x table) at a 0.75 load factor.
// The count is per-isolate (each isolate owns its own table) and is recomputed
// lazily on first use — the snapshot restores the table but not this C counter.
static PER_ISOLATE size_t gSymbolCount = 0;
static PER_ISOLATE _Bool gSymbolCountValid = 0;

static void symbolTableRecount(void)
{
	RawArray *table = (RawArray *) Handles.SymbolTable->raw;
	size_t size = table->size;
	size_t count = 0;
	for (size_t i = 0; i < size; i++) {
		if (asObject(table->vars[i]) != Handles.nil->raw) {
			count++;
		}
	}
	gSymbolCount = count;
	gSymbolCountValid = 1;
}

// Allocate a table twice as large and rehash every existing symbol into it by
// its stored hash, then repoint the (snapshot-serialized) SymbolTable handle so
// the whole isolate switches to the larger table.
static void growSymbolTable(void)
{
	HandleScope scope;
	openHandleScope(&scope);

	Array *oldTable = scopeHandle(Handles.SymbolTable->raw);
	size_t oldSize = ((RawArray *) oldTable->raw)->size;
	size_t newSize = oldSize * 2;
	Array *newTable = scopeHandle(newArray(newSize)->raw); // may GC; oldTable is rooted

	// Rehash. No allocation happens in this loop, so raw pointers stay stable
	// and slot writes are young(newTable) <- old(symbol): barrier-safe.
	RawArray *from = (RawArray *) oldTable->raw;
	for (size_t i = 0; i < oldSize; i++) {
		RawObject *rawSym = asObject(from->vars[i]);
		if (rawSym == Handles.nil->raw) {
			continue;
		}
		size_t idx = (((RawString *) rawSym)->hash & 0xFFFFFFFF) & (newSize - 1);
		while (asObject(((RawArray *) newTable->raw)->vars[idx]) != Handles.nil->raw) {
			idx = idx == newSize - 1 ? 0 : idx + 1;
		}
		Object symBox;
		symBox.raw = rawSym;
		arrayAtPutObject(newTable, idx, &symBox);
	}

	Handles.SymbolTable->raw = newTable->raw; // the VM now interns into the big table
	closeHandleScope(&scope, NULL);
	// Keep the Smalltalk-level `SymbolTable` global in sync. Safe from re-entrant
	// growth: the load factor is now ~0.375, so this asSymbol call cannot re-grow.
	setGlobalObject("SymbolTable", (Object *) Handles.SymbolTable);
}

String *asSymbol(String *string)
{
	HandleScope scope;
	openHandleScope(&scope);

	if (string->raw->class != Handles.String->raw) {
		FAIL();
	}
	if (!gSymbolCountValid) {
		symbolTableRecount();
	}

	String *symbol = scopeHandle(Handles.nil->raw);
	Value hash = computeStringHash(string) & 0xFFFFFFFF;

	RawArray *table = (RawArray *) Handles.SymbolTable->raw;
	Value size = table->size;
	size_t index = hash & size - 1;
	for (;;) {
		symbol->raw = (RawString *) asObject(table->vars[index]);
		if (isNil(symbol)) {
			break; // empty slot: this is a brand-new symbol
		}
		if (stringEquals(string, symbol)) {
			return closeHandleScope(&scope, symbol); // already interned
		}
		index = index == size - 1 ? 0 : index + 1;
	}

	// Interning a new symbol. Grow first if it would push past the load factor,
	// then re-probe for a free slot in the (possibly new) table.
	if ((gSymbolCount + 1) * 4 >= (size_t) size * 3) {
		growSymbolTable();
		table = (RawArray *) Handles.SymbolTable->raw;
		size = table->size;
		index = hash & size - 1;
		while (asObject(table->vars[index]) != Handles.nil->raw) {
			index = index == (size_t) size - 1 ? 0 : index + 1;
		}
	}

	String *newSymbol = (String *) copyResizedObject((Object *) string, string->raw->size);
	newSymbol->raw->class = Handles.Symbol->raw;
	newSymbol->raw->hash = hash;
	arrayAtPutObject(Handles.SymbolTable, index, (Object *) newSymbol);
	gSymbolCount++;
	return closeHandleScope(&scope, newSymbol);
}


String *getSymbol(char *string)
{
	return asSymbol(asString(string));
}


void setGlobal(char *key, Value value)
{
	HandleScope scope;
	openHandleScope(&scope);
	symbolDictAtPut(Handles.Smalltalk, getSymbol(key), value);
	closeHandleScope(&scope, NULL);
}


void setGlobalObject(char *key, Object *value)
{
	HandleScope scope;
	openHandleScope(&scope);
	symbolDictAtPutObject(Handles.Smalltalk, getSymbol(key), value);
	closeHandleScope(&scope, NULL);
}


Value getGlobal(char *key)
{
	return symbolDictAt(Handles.Smalltalk, getSymbol(key));
}


Object *getGlobalObject(char *key)
{
	return symbolDictObjectAt(Handles.Smalltalk, getSymbol(key));
}


void globalAtPut(String *key, Value value)
{
	symbolDictAtPut(Handles.Smalltalk, key, value);
}


Value globalAt(String *key)
{
	return symbolDictAt(Handles.Smalltalk, key);
}


Object *globalObjectAt(String *key)
{
	return scopeHandle(asObject(globalAt(key)));
}


Class *getClass(char *key)
{
	return (Class *) getGlobalObject(key);
}


void objectBecome(Object *old, Object *new)
{
	size_t oldSize = computeObjectSize(old);
	size_t newSize = computeObjectSize(new);

	if (oldSize == newSize) {
		memcpy(old->raw, new->raw, newSize);
	} else {
		swapObjectPointers(old, new);
	}
}


static void swapObjectPointers(Object *old, Object *new)
{
	swapObjectInNewSpace(old, new);
	swapObjectInOldSpace(old, new);
	swapObjectOnStack(old, new);
}


static void swapObjectInNewSpace(Object *old, Object *new)
{
	size_t objects = 0;
	RawObject *object = (RawObject *) ((uintptr_t) CurrentThread.heap.newSpace.fromSpace | NEW_SPACE_TAG);
	RawObject *prev = NULL;
	// The young allocation high-water now lives in the mutator's TLAB, not
	// newSpace.top (which was advanced when the TLAB carved its chunk).
	RawObject *end = (RawObject *) CurrentThread.tlab.top;
	while (object < end) {
		objects++;
		iterateObject(object, old, new);
		prev = object;
		object = (RawObject *) ((uint8_t *) object + align(computeRawObjectSize(object), HEAP_OBJECT_ALIGN));
	}
}


static void swapObjectInOldSpace(Object *old, Object *new)
{
	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &CurrentThread.heap.oldSpace);
	RawObject *object = pageSpaceIteratorNext(&iterator);
	while (object != NULL) {
		if ((object->tags & TAG_FREESPACE) != 0) {
			object = pageSpaceIteratorNext(&iterator);
			continue;
		}
		iterateObject(object, new, old);
		object = pageSpaceIteratorNext(&iterator);
	}
}


static void swapObjectOnStack(Object *old, Object *new)
{
	Value tOld = getTaggedPtr(old);
	Value tNew = getTaggedPtr(new);
	EntryStackFrame *entryFrame = CurrentThread.stackFramesTail;

	while (entryFrame != NULL) {
		StackFrame *prev = entryFrame->exit;
		StackFrame *frame = stackFrameGetParent(prev, entryFrame);
		while (frame != NULL) {
			NativeCode *code = stackFrameGetNativeCode(frame);

			size_t argsSize = ((RawCompiledMethod *) code->compiledCode)->header.argsSize + 1;
			for (ptrdiff_t i = 0; i < argsSize; i++) {
				Value value = stackFrameGetArg(frame, i);
				if (value == tOld) {
					stackFrameSetArg(frame, i, tNew);
				}
			}

			RawStackmap *stackmap = findStackmap(code, (ptrdiff_t) prev->parentIc);
			ASSERT(stackmap != NULL);
			size_t frameSize = (stackmap->size - sizeof(Value)) * 8;
			for (size_t i = 0; i < frameSize; i++) {
				//ASSERT(i != 0 || stackmapIncludes(stackmap, i));
				if (stackmapIncludes(stackmap, i)) {
					Value value = stackFrameGetSlot(frame, i);
					if (value == tOld) {
						stackFrameSetSlot(frame, i, tNew);
					}
				}
			}

			prev = frame;
			frame = stackFrameGetParent(frame, entryFrame);
		}
		entryFrame = entryFrame->prev;
	}
}


static void iterateObject(RawObject *object, Object *old, Object *new)
{
	ASSERT(((intptr_t) object->class & 3) == 0);
	Value *vars = getRawObjectVars(object);
	size_t size = object->class->instanceShape.varsSize;
	if (object->class->instanceShape.isIndexed && !object->class->instanceShape.isBytes) {
		size += rawObjectSize(object);
	}
	// TODO: allow swaping object class?
	//if (object->class == (RawClass *) old->raw) {
	//	object->class = (RawClass *) new->raw;
	//}
	for (size_t i = 0; i < size; i++) {
		if (vars[i] == getTaggedPtr(old)) {
			rawObjectStorePtr(object, &vars[i], new->raw);
		}
	}
}
