// Allocation.
//
// Two of the few primitives that touch the heap, and therefore two of the few
// that ANCHOR THE CALLING FRAME. Allocating can trigger a collection, and a
// collection has to be able to walk the compiled frames underneath this
// primitive or it will evacuate objects that live methods are still holding in
// their registers.

#include "runtime/primitives/Shared.h"
#include "memory/ObjectWalk.h"


static Value allocateInstance(Value *args, uint64_t elements)
{
	HandleScope scope;
	openHandleScope(&scope);
	Class *class = receiverAsClass(primitiveReceiver(args));
	if (class == NULL) {
		closeHandleScope(&scope, NULL);
		return PRIMITIVE_FAILED;
	}
	Object *instance = newObject(class, elements);
	// NIL, not the zero the allocator leaves. Smalltalk says a fresh instance
	// variable answers nil, and zero is the SmallInteger 0; the allocator
	// deliberately does not do this, because inside the VM an unset slot means
	// ABSENT and nil is an object (memory/Heap.c). This is the boundary where
	// the Smalltalk rule starts applying.
	//
	// The range comes from objectPointerSlots, so it is by construction the same
	// range the collector scans: raw words and byte bodies are left alone.
	// nil is IMMORTAL, so storing it needs no write barrier.
	if (formatHasPointers(rawObjectFormat(instance->raw))) {
		size_t count;
		Value *slots = objectPointerSlots(&CurrentThread.heap->classes,
			instance->raw, &count);
		for (size_t i = 0; i < count; i++) {
			slots[i] = tagPtr(Handles.nil.raw);
		}
	}
	Value answer = objectTagged(instance);
	closeHandleScope(&scope, NULL);
	// Read out BEFORE returning and with nothing allocating in between: the
	// scope is closed, so `answer` is only valid until the next allocation, and
	// the very next thing that happens is a store into a frame slot.
	return answer;
}


Value primBasicNew(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = allocateInstance(args, 0);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primBasicNewSized(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value size = primitiveArgument(args, 0);
	if (!valueTypeOf(size, VALUE_INT) || asCInt(size) < 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = allocateInstance(args, (uint64_t) asCInt(size));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
