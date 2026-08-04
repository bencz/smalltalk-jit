#include "runtime/Closure.h"
#include "core/Thread.h"
#include "memory/Heap.h"


Closure *newClosure(Object *method, uint16_t captureCount, uint64_t homeToken)
{
	HandleScope scope;
	openHandleScope(&scope);
	// The method goes in through a HANDLE and is stored AFTER the allocation,
	// because allocating the closure can collect and move it.
	Object *held = scopeHandle(method->raw);
	Closure *closure = newObject(&Handles.Closure, captureCount);
	rawObjectStorePtr((RawObject *) closure->raw, &closure->raw->method, held->raw);
	// Tagged, so the object stays uniformly tagged and no barrier is involved:
	// a token is a number naming an activation, not a pointer to one.
	closure->raw->homeToken = tagInt((intptr_t) homeToken);
	// Every capture slot starts as nil rather than as zero: a slot the emitter
	// has not filled yet must still be a legal Value if a collection lands
	// between the allocation and the stores.
	for (uint16_t i = 0; i < captureCount; i++) {
		closure->raw->captured[i] = tagPtr(Handles.nil.raw);
	}
	return closeHandleScope(&scope, closure);
}


Cell *newCell(Value value)
{
	HandleScope scope;
	openHandleScope(&scope);
	// The value is a tagged Value and may be a pointer to a young object, so it
	// is re-read from a root after the allocation rather than held in a C local
	// across it. Wrapping it costs one handle and removes the whole question.
	Object *held = valueTypeOf(value, VALUE_POINTER)
		? (Object *) scopeHandle(asObject(value)) : NULL;
	Cell *cell = newObject(&Handles.Cell, 0);
	cell->raw->value = held != NULL ? tagPtr(held->raw) : value;
	return closeHandleScope(&scope, cell);
}
