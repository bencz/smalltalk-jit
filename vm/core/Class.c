#include "core/Class.h"
#include "core/Assert.h"
#include "memory/Heap.h"
#include "runtime/String.h"

ImmediateClassIndices gImmediateClasses;


RawClass *classOf(Value value)
{
	return (RawClass *) classTableAt(&CurrentThread.heap->classes,
		classIndexOfValue(value));
}


Class *classCreate(Class *superclass, union String *name, InstanceShape shape)
{
	HandleScope scope;
	openHandleScope(&scope);

	// A class object is FORMAT_MIXED_BYTES: its tagged fields, then a raw
	// trailer holding the shape it stamps and its own index. Describing it that
	// way rather than as all-pointers is what stops the collector from trying to
	// follow the shape bits as if they were a reference.
	//
	// The trailer size is derived from the C struct rather than written down, so
	// adding a field to RawClass cannot silently under-allocate every class in
	// the system.
	Class *class = newObject(&Handles.ClassClass, CLASS_RAW_TRAILER_BYTES);

	uint32_t index = classTableAdd(&CurrentThread.heap->classes,
		(RawObject *) class->raw);
	class->raw->classIndex = index;
	class->raw->instanceShape = shape;

	if (superclass != NULL) {
		rawObjectStorePtr((RawObject *) class->raw, &class->raw->superClass,
			(RawObject *) superclass->raw);
	}
	if (name != NULL) {
		rawObjectStorePtr((RawObject *) class->raw, &class->raw->name,
			(RawObject *) name->raw);
	}
	return closeHandleScope(&scope, class);
}
