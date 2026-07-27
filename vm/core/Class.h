#ifndef CLASS_H
#define CLASS_H

// Classes.
//
// A class is an ordinary movable object like any other. What is STABLE about it
// is its index in the heap's class table (ADR 0005), and that is what generated
// code bakes, what an inline cache compares, and what an object header carries.
// Nothing outside this file should hold a RawClass* across an allocation.

#include "core/ClassTable.h"
#include "core/Handle.h"
#include "core/Object.h"

// The class of any value, immediate or heap. The ONE place that knows tagged
// immediates have classes too.
RawClass *classOf(Value value);

// Just the index, which is what every fast path actually wants: for a heap
// object it is a field read with no indirection at all.
static inline uint32_t classIndexOfValue(Value value);

// Create a class: allocates the object, assigns it a class-table index, and
// records the shape it will stamp onto instances. `superclass` may be NULL for
// the root.
union String;
Class *classCreate(Class *superclass, union String *name, InstanceShape shape);

// How a class object describes ITSELF: the tagged fields, then a raw trailer.
// Both numbers come from the C struct, so a field added to RawClass changes
// them automatically instead of leaving every class one field short.
#define CLASS_TAGGED_FIELDS 9
#define CLASS_RAW_TRAILER_BYTES \
	(sizeof(RawClass) - HEADER_SIZE - CLASS_TAGGED_FIELDS * sizeof(Value))
#define CLASS_OF_CLASSES_SHAPE \
	((InstanceShape) DEFINE_SHAPE(FORMAT_MIXED_BYTES, 0, CLASS_TAGGED_FIELDS, 0))

// The shape a class stamps onto its instances.
static inline InstanceShape classShape(Class *class)
{
	return class->raw->instanceShape;
}


// Indices of the immediate classes, resolved once at bootstrap. They cannot be
// found from an object header, because an immediate has no header.
typedef struct {
	uint32_t smallInteger;
	uint32_t character;
	uint32_t smallFloat;
} ImmediateClassIndices;

extern ImmediateClassIndices gImmediateClasses;


static inline uint32_t classIndexOfValue(Value value)
{
	switch (value & 3) {
	case VALUE_INT:
		return gImmediateClasses.smallInteger;
	case VALUE_CHAR:
		return gImmediateClasses.character;
	case VALUE_FLOAT:
		return gImmediateClasses.smallFloat;
	default:
		return rawObjectClassIndex(asObject(value));
	}
}

#endif
