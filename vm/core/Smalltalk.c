#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/StackFrame.h"
#include "jit/CodeDescriptors.h"
#include "core/Thread.h"
#include "concurrency/Scheduler.h"
#include "concurrency/Fiber.h"
#include "core/Assert.h"
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
// The count is PER-HEAP (the table is shared by all workers of a heap; a per-thread
// counter would let workers disagree on the load factor and corrupt the shared table)
// and is recomputed lazily on first use — the snapshot restores the table but not it.
// All accesses below run under heapSymbolLockEnter (asSymbol), so no extra sync needed.
#define gSymbolCount      (CurrentThread.heap->symbolCount)
#define gSymbolCountValid (CurrentThread.heap->symbolCountValid)

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
	// Everything below touches the ONE shared symbol table + occupancy counter, so it runs
	// under the per-heap symbol lock (re-entrant: growSymbolTable → setGlobal → asSymbol).
	Heap *heap = CurrentThread.heap;
	heapSymbolLockEnter(heap);

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
			heapSymbolLockLeave(heap);
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
	heapSymbolLockLeave(heap);
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
	Heap *heap = CurrentThread.heap;
	// become: walks the whole heap + every mutator/fiber stack and rewrites pointers, so
	// it must run with a STABLE object graph: no peer allocating (which would extend the
	// young space under our walk) or collecting (moving objects). Stop the world exactly
	// like heapCollectYoung — take gcLock (waiting counts as GC-safe), then park every
	// other mutator at a safepoint. Single-mutator (no peers): gcLock alone suffices, no
	// safepoint handshake needed. become: allocates nothing, so holding gcLock is deadlock-free.
	_Bool multi = (heap->mutators != NULL && heap->mutators->nextMutator != NULL);
	heapGcEnterBlocked(heap, &CurrentThread);
	pthread_mutex_lock(&heap->gcLock);
	heapGcLeaveBlocked(heap, &CurrentThread);
	if (multi) {
		heapGcBegin(heap, &CurrentThread);
	}
	// Make the young space linearly walkable: retire every parked mutator's TLAB tail
	// (the current thread's included) into a filler so swapObjectInNewSpace can skip them.
	heapFillAllTlabTails(heap);

	size_t oldSize = computeObjectSize(old);
	size_t newSize = computeObjectSize(new);
	if (oldSize == newSize) {
		memcpy(old->raw, new->raw, newSize);
	} else {
		swapObjectPointers(old, new);
	}

	if (multi) {
		heapGcEnd(heap);
	}
	pthread_mutex_unlock(&heap->gcLock);
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
	RawObject *object = (RawObject *) ((uintptr_t) CurrentThread.heap->newSpace.fromSpace | NEW_SPACE_TAG);
	RawObject *prev = NULL;
	// Walk the WHOLE carved young space up to the shared high-water (newSpace.top), NOT
	// this thread's tlab.top — under the multi-worker pool objects live in EVERY mutator's
	// TLAB, whose tails objectBecome just retired to fillers (heapFillAllTlabTails), so the
	// span [fromSpace, newSpace.top) is fully parseable and skips those retired tails.
	RawObject *end = (RawObject *) CurrentThread.heap->newSpace.top;
	while (object < end) {
		if ((object->tags & TAG_FREESPACE) != 0) { // retired TLAB tail: skip
			object = (RawObject *) ((uint8_t *) object + ((FreeSpace *) object)->size);
			continue;
		}
		objects++;
		iterateObject(object, old, new);
		prev = object;
		object = (RawObject *) ((uint8_t *) object + align(computeRawObjectSize(object), HEAP_OBJECT_ALIGN));
	}
}


static void swapObjectInOldSpace(Object *old, Object *new)
{
	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &CurrentThread.heap->oldSpace);
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


// Rewrite every tOld reference to tNew in one entry-frame chain (args + JIT stack slots
// via the frame's stackmap). Factored out of swapObjectOnStack so become: can apply it to
// EVERY mutator's live stack AND every parked fiber's saved stack, not just the caller's.
static void swapStackFrames(EntryStackFrame *entryFrame, Value tOld, Value tNew)
{
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


static void swapObjectOnStack(Object *old, Object *new)
{
	Value tOld = getTaggedPtr(old);
	Value tNew = getTaggedPtr(new);

	// Always scan the current thread's live stack (also covers bootstrap, before any
	// scheduler / mutator registry exists).
	swapStackFrames(CurrentThread.stackFramesTail, tOld, tNew);
	if (!schedulerActive()) {
		return;
	}
	// Under the multi-worker pool the old pointer can be live on ANY peer worker's stack or
	// in ANY parked fiber's saved stack, so scan them all (STW point → every stack is
	// quiescent). Peers via heap->mutators (skip self, already done above); parked fibers via
	// their saved roots.stackFramesTail (a RUNNING fiber's live stack is its worker's TLS,
	// already covered by the mutator loop, so skip it to avoid processing a chain twice).
	for (struct Thread *m = CurrentThread.heap->mutators; m != NULL; m = m->nextMutator) {
		if (m != &CurrentThread) {
			swapStackFrames(m->stackFramesTail, tOld, tNew);
		}
	}
	size_t slots = schedulerFiberSlots();
	for (size_t i = 0; i < slots; i++) {
		Fiber *f = schedulerFiberAt(i);
		if (f != NULL && f->state != FIBER_RUNNING) {
			swapStackFrames(f->roots.stackFramesTail, tOld, tNew);
		}
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
