#include "core/Handle.h"
#include "core/Assert.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "core/Thread.h"
#include <stdlib.h>
#include <string.h>


void initHandles(void)
{
	CurrentThread.handleScopes = NULL;
}


void freeHandles(void)
{
	// Scopes live on the C stack; only their overflow chunks are heap. A
	// non-empty chain here means a scope was not closed, which is a bug in the
	// caller rather than something to clean up silently.
	ASSERT(CurrentThread.handleScopes == NULL);
}


void openHandleScope(HandleScope *scope)
{
	scope->parent = CurrentThread.handleScopes;
	scope->size = 0;
	scope->overflow = NULL;
	CurrentThread.handleScopes = scope;
}


static Object *scopeSlot(HandleScope *scope, size_t index)
{
	if (index < HANDLE_SCOPE_INLINE) {
		return &scope->handles[index];
	}
	// Chunks are newest-first and all but the newest are full, so the index of
	// a slot inside its chunk is what is left after the inline part and the
	// chunks NEWER than it. Walking from the newest is therefore correct.
	size_t beyond = index - HANDLE_SCOPE_INLINE;
	size_t chunkCount = (scope->size - HANDLE_SCOPE_INLINE + HANDLE_CHUNK_SIZE - 1)
		/ HANDLE_CHUNK_SIZE;
	size_t chunkIndex = beyond / HANDLE_CHUNK_SIZE;
	size_t within = beyond % HANDLE_CHUNK_SIZE;
	HandleChunk *chunk = scope->overflow;
	for (size_t i = chunkIndex + 1; i < chunkCount && chunk != NULL; i++) {
		chunk = chunk->next;
	}
	ASSERT(chunk != NULL);
	return &chunk->handles[within];
}


void *outermostHandle(void *rawObject)
{
	HandleScope *inner = CurrentThread.handleScopes;
	ASSERT(inner != NULL);
	HandleScope *outer = inner;
	while (outer->parent != NULL) {
		outer = outer->parent;
	}
	// scopeHandle allocates in whatever CurrentThread says is innermost, so the
	// outermost is made innermost for exactly one call. No allocation happens
	// in between, so nothing observes the swap.
	CurrentThread.handleScopes = outer;
	void *handle = scopeHandle(rawObject);
	CurrentThread.handleScopes = inner;
	return handle;
}


void *scopeHandle(void *rawObject)
{
	HandleScope *scope = CurrentThread.handleScopes;
	ASSERT(scope != NULL); // a handle with no open scope has no owner
	size_t index = scope->size;
	if (index >= HANDLE_SCOPE_INLINE
			&& (index - HANDLE_SCOPE_INLINE) % HANDLE_CHUNK_SIZE == 0) {
		HandleChunk *chunk = malloc(sizeof(HandleChunk));
		ASSERT(chunk != NULL);
		chunk->next = scope->overflow;
		scope->overflow = chunk;
	}
	scope->size = index + 1;
	Object *handle = scopeSlot(scope, index);
	handle->raw = rawObject;
	return handle;
}


void *closeHandleScope(HandleScope *scope, void *result)
{
	ASSERT(CurrentThread.handleScopes == scope);
	// Read the result BEFORE the scope goes away: `result` points INTO this
	// scope, so promoting it means copying the value out and re-handling it in
	// the parent.
	void *raw = result != NULL ? ((Object *) result)->raw : NULL;

	HandleChunk *chunk = scope->overflow;
	while (chunk != NULL) {
		HandleChunk *older = chunk->next;
		free(chunk);
		chunk = older;
	}
	scope->overflow = NULL;
	scope->size = 0;
	CurrentThread.handleScopes = scope->parent;

	// Nothing to promote INTO when this was the outermost scope. Returning NULL
	// rather than asserting: closing the last scope with a result is what the
	// top of a call chain does, and the caller is about to stop using handles
	// anyway. Promoting unconditionally re-handled into a scope that no longer
	// existed, which is how the self-test found this.
	if (raw == NULL || CurrentThread.handleScopes == NULL) {
		return NULL;
	}
	return scopeHandle(raw);
}


void *newObject(Class *class, size_t elements)
{
	// The class is read through its handle, so an allocation that triggers a
	// collection and moves the class object cannot leave this holding a stale
	// pointer. That ordering is the whole reason this function exists rather
	// than callers using allocateObject directly.
	RawObject *object = allocateObject(CurrentThread.heap,
		(RawObject *) class->raw, elements);
	return scopeHandle(object);
}


void *newImmortalObject(Class *class, size_t elements)
{
	return scopeHandle(allocateImmortalObject(CurrentThread.heap,
		(RawObject *) class->raw, elements));
}


Object *copyResizedObject(Object *object, size_t elements)
{
	HandleScope scope;
	openHandleScope(&scope);

	// Allocate FIRST, then re-read the source through its handle: the allocation
	// can collect and move it, and copying out of a pointer taken before that
	// would read the corpse.
	Class *class = scopeHandle(classTableAt(&CurrentThread.heap->classes,
		rawObjectClassIndex(object->raw)));
	Object *copy = newObject(class, elements);

	ObjectFormat format = rawObjectFormat(object->raw);
	size_t had = rawObjectElementCount(object->raw);
	size_t take = had < elements ? had : elements;
	size_t unit = format == FORMAT_BYTES ? 1
		: format == FORMAT_DOUBLES ? sizeof(double) : sizeof(Value);
	// Named slots come before the indexed run in every format that has both, so
	// they are copied as part of the same contiguous region.
	size_t fixed = (size_t) class->raw->instanceShape.fixedSlots * sizeof(Value);
	memcpy(objectIndexedVars(copy), objectIndexedVars(object), fixed + take * unit);
	// Tagged elements may be young while the copy is old, so the barrier has to
	// see every one of them. memcpy moved the bits; this is what makes them
	// visible to the next collection.
	if (format == FORMAT_INDEXED_POINTERS) {
		Value *slots = (Value *) objectIndexedVars(copy);
		for (size_t i = 0; i < fixed / sizeof(Value) + take; i++) {
			rawObjectStoreValue(copy->raw, &slots[i], slots[i]);
		}
	}
	return closeHandleScope(&scope, copy);
}


void handlesVisitRoots(struct Thread *thread, RootVisitor visit, void *ctx)
{
	for (HandleScope *scope = thread->handleScopes; scope != NULL;
			scope = scope->parent) {
		size_t count = scope->size;
		size_t inline_ = count < HANDLE_SCOPE_INLINE ? count : HANDLE_SCOPE_INLINE;
		for (size_t i = 0; i < inline_; i++) {
			if (scope->handles[i].raw != NULL) {
				Value slot = tagPtr(scope->handles[i].raw);
				visit(ctx, &slot);
				scope->handles[i].raw = asObject(slot);
			}
		}
		// The overflow chain, newest first. Every chunk but the newest is full;
		// the newest holds whatever is left over.
		size_t remaining = count > HANDLE_SCOPE_INLINE
			? count - HANDLE_SCOPE_INLINE : 0;
		size_t newest = remaining % HANDLE_CHUNK_SIZE;
		if (remaining > 0 && newest == 0) {
			newest = HANDLE_CHUNK_SIZE;
		}
		for (HandleChunk *chunk = scope->overflow; chunk != NULL && remaining > 0;
				chunk = chunk->next) {
			for (size_t i = 0; i < newest; i++) {
				if (chunk->handles[i].raw != NULL) {
					Value slot = tagPtr(chunk->handles[i].raw);
					visit(ctx, &slot);
					chunk->handles[i].raw = asObject(slot);
				}
			}
			remaining -= newest;
			newest = remaining < HANDLE_CHUNK_SIZE ? remaining : HANDLE_CHUNK_SIZE;
		}
	}
}


void smalltalkHandlesVisitRoots(struct Heap *heap, RootVisitor visit, void *ctx)
{
	// Flat slot walk rather than field-by-field. A well-known class added to
	// SmalltalkHandles is then scanned automatically, instead of dangling until
	// someone notices it was left out of a hand-written list.
	RawObject **slots = (RawObject **) heap->handles;
	size_t count = smalltalkHandleSlotCount();
	for (size_t i = 0; i < count; i++) {
		if (slots[i] == NULL) {
			continue; // not yet bootstrapped
		}
		Value slot = tagPtr(slots[i]);
		visit(ctx, &slot);
		slots[i] = asObject(slot);
	}
}
