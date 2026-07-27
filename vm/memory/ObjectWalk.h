#ifndef OBJECT_WALK_H
#define OBJECT_WALK_H

// The two questions the collector asks about every object it touches:
// how big is it, and where are its pointers.
//
// Both are answered from the OBJECT for every format the collector walks in
// bulk: ordinary objects, Arrays, Strings, and the flat value arrays of phase 7.
// The old collector paid two dependent loads per object (object -> class ->
// shape) just to learn a size.
//
// Two narrow exceptions, both deliberate and both rare:
//
//   * the MIXED formats need the class to know where their tagged run begins.
//     That is CompiledMethod, CompiledBlock, Block, Context, ExceptionHandler;
//   * an object too large for the header's 8-bit size field derives its size
//     from its element count when it is indexed, and from its class when it is
//     not. NOTHING carries a size prefix word. The first design did, and it was
//     a word the allocator never reserved: the self-test found it immediately,
//     as a size of (size_t) -2 on the 1024-element symbol table.

#include "core/ClassTable.h"
#include "core/Object.h"


// Round a byte size up to the heap's object alignment. Every allocator path
// goes through this: the young/old discrimination is a bit of the ADDRESS
// (core/Object.h), so under-aligning an object silently mislabels its
// generation.
static inline size_t objectAlignSize(size_t bytes)
{
	return (bytes + (HEAP_OBJECT_ALIGN - 1)) & ~(size_t) (HEAP_OBJECT_ALIGN - 1);
}


// Bytes an instance of `shape` with `elements` indexed elements occupies,
// header included and alignment applied. The single place that knows how a
// shape turns into a size; the allocator and the snapshot loader both use it,
// and a divergence between them would produce objects the collector strides
// past incorrectly.
static inline size_t objectSizeForShape(InstanceShape shape, size_t elements)
{
	size_t bytes = HEADER_SIZE;
	switch ((ObjectFormat) shape.format) {
	case FORMAT_NO_POINTERS:
	case FORMAT_POINTERS:
		bytes += (size_t) shape.fixedSlots * sizeof(Value);
		break;
	case FORMAT_INDEXED_POINTERS:
		bytes += sizeof(uint64_t)                                  // element count
			+ ((size_t) shape.fixedSlots + elements) * sizeof(Value);
		break;
	case FORMAT_BYTES:
		bytes += sizeof(uint64_t) + elements;
		break;
	case FORMAT_DOUBLES:
		bytes += sizeof(uint64_t) + elements * sizeof(double);
		break;
	case FORMAT_MIXED_POINTERS:
		// `elements` counts TAGGED slots here (a Context's variables).
		bytes += ((size_t) shape.rawWords + shape.fixedSlots + elements)
			* sizeof(Value);
		break;
	case FORMAT_MIXED_BYTES:
		// `elements` counts BYTES here (a method's bytecodes).
		bytes += ((size_t) shape.rawWords + shape.pointerWords) * sizeof(Value)
			+ elements;
		break;
	default:
		FAIL();
	}
	return objectAlignSize(bytes);
}


// Total size in bytes, header included. The collector strides on this.
static inline size_t objectSizeInBytes(ClassTable *classes, RawObject *object)
{
	ObjectFormat format = rawObjectFormat(object);
	switch (format) {
	case FORMAT_INDEXED_POINTERS:
		return objectAlignSize(HEADER_SIZE + sizeof(uint64_t)
			+ rawObjectElementCount(object) * sizeof(Value));
	case FORMAT_BYTES:
		return objectAlignSize(HEADER_SIZE + sizeof(uint64_t)
			+ rawObjectElementCount(object));
	case FORMAT_DOUBLES:
		return objectAlignSize(HEADER_SIZE + sizeof(uint64_t)
			+ rawObjectElementCount(object) * sizeof(double));
	default:
		break;
	}
	size_t words = rawObjectInlineSizeWords(object);
	if (words != SIZE_WORDS_BIG) {
		return words * sizeof(uint64_t);
	}
	// Too large for the field and not indexed by a count readable from here:
	// the class knows the shape. Rare by construction, because the fixed
	// formats are bounded by a class's field count.
	ASSERT(classes != NULL);
	RawClass *class = (RawClass *) classTableAt(classes, rawObjectClassIndex(object));
	size_t elements = format == FORMAT_MIXED_BYTES ? rawObjectElementCount(object) : 0;
	return objectSizeForShape(class->instanceShape, elements);
}


// Body words available after the header, whatever the format uses them for.
static inline size_t objectBodyWords(ClassTable *classes, RawObject *object)
{
	return objectSizeInBytes(classes, object) / sizeof(uint64_t) - 1;
}


// Where an object's tagged slots begin and how many there are. `count` is 0 for
// every format that holds no pointer, and the caller must not dereference the
// returned base in that case.
static inline Value *objectPointerSlots(ClassTable *classes, RawObject *object,
	size_t *count)
{
	Value *body = (Value *) object->body;
	switch (rawObjectFormat(object)) {
	case FORMAT_POINTERS:
		*count = objectBodyWords(classes, object);
		return body;

	case FORMAT_INDEXED_POINTERS:
		// Body word 0 is the element count, not a slot.
		*count = objectBodyWords(classes, object) - 1;
		return body + 1;

	case FORMAT_MIXED_POINTERS: {
		// Raw words, then tagged to the END of the object: Context, whose
		// tagged run is as long as the activation needs, and Block.
		ASSERT(classes != NULL);
		RawClass *class = (RawClass *) classTableAt(classes, rawObjectClassIndex(object));
		size_t raw = class->instanceShape.rawWords;
		*count = objectBodyWords(classes, object) - raw;
		return body + raw;
	}

	case FORMAT_MIXED_BYTES: {
		// Raw words, a FIXED tagged run, then bytes: CompiledMethod and
		// CompiledBlock, whose bytecodes follow the literal frame.
		ASSERT(classes != NULL);
		RawClass *class = (RawClass *) classTableAt(classes, rawObjectClassIndex(object));
		*count = class->instanceShape.pointerWords;
		return body + class->instanceShape.rawWords;
	}

	case FORMAT_NO_POINTERS:
	case FORMAT_BYTES:
	case FORMAT_DOUBLES:
	case FORMAT_FORWARDED:
	case FORMAT_FREE:
	default:
		*count = 0;
		return body;
	}
}

#endif
