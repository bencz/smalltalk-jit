#include "core/Class.h"
#include "core/Assert.h"
#include "memory/Heap.h"
#include "runtime/String.h"

// CLASS_INDEX_INVALID everywhere until bootstrap fills it, VALUE_POINTER's
// entry for good: a value with that tag is answered from its header and never
// from here.
uint32_t gClassIndexByTag[4] = {
	CLASS_INDEX_INVALID, CLASS_INDEX_INVALID,
	CLASS_INDEX_INVALID, CLASS_INDEX_INVALID,
};


void classSetImmediateIndices(uint32_t smallInteger, uint32_t character,
	uint32_t smallFloat)
{
	gClassIndexByTag[VALUE_INT] = smallInteger;
	gClassIndexByTag[VALUE_CHAR] = character;
	gClassIndexByTag[VALUE_FLOAT] = smallFloat;
	// Left alone on purpose, and asserted rather than assumed: the emitted
	// inline cache indexes this table by a tag it has already tested is not
	// VALUE_POINTER, and this is what makes a mistake there miss instead of
	// matching a class that happens to sit at index 0.
	ASSERT(gClassIndexByTag[VALUE_POINTER] == CLASS_INDEX_INVALID);
}


RawClass *classOf(Value value)
{
	return (RawClass *) classTableAt(&CurrentThread.heap->classes,
		classIndexOfValue(value));
}


// The tagged fields of the class mirror that ONLY SMALLTALK reads answer nil
// when nothing set them, because nil is the only absent Smalltalk has.
//
// The VM's own absent is the allocator's ZERO (core/Object.h), which is
// `tagInt(0)`, and Smalltalk tests these fields with `isNil`. So an unset field
// came back as the SmallInteger 0, `isNil` answered false, and the code went on
// as if a value were there: `Class>>qualifiedName` asks `namespace isNil`, got
// false for every kernel class, and went on to index a collection that does not
// exist.
//
// FOUR FIELDS ARE DELIBERATELY NOT HERE. `name`, `methodDictionary`,
// `instanceVariables` and `classVariables` are read in C as "is it a pointer",
// and that test IS the VM's presence check -- nil is a pointer, so filling them
// would turn "this class has no method dictionary" into "its method dictionary
// is the nil object". Those four stay zero, and every one of them is written by
// the class builder for every class it builds.
//
// Does nothing before nil exists: the first classes are made during bootstrap,
// before there is an UndefinedObject to instantiate. They are all reopened by
// packages/Core, and the class builder calls this again on the way through.
void classFillAbsentSmalltalkFields(Class *class)
{
	if (Handles.nil.raw == NULL) {
		return;
	}
	Value *fields[] = {
		&class->raw->subClasses, &class->raw->comment,
		&class->raw->category, &class->raw->namespace,
	};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (!valueTypeOf(*fields[i], VALUE_POINTER)) {
			rawObjectStorePtr((RawObject *) class->raw, fields[i], Handles.nil.raw);
		}
	}
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
	classFillAbsentSmalltalkFields(class);
	return closeHandleScope(&scope, class);
}
