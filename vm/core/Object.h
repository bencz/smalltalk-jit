#ifndef OBJECT_H
#define OBJECT_H

#include "core/Assert.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OBJECT_HANDLE(name) \
	typedef union name { \
		Raw##name *raw; \
		uintptr_t ptr; \
	} name

#define HEADER_SIZE (2 * sizeof(Value))
#define OBJECT_HEADER \
	struct RawClass *class; \
	uint32_t hash; \
	uint8_t unused; \
	uint8_t payloadSize; \
	uint8_t varsSize; \
	uint8_t tags

// PORT_ME(addr-tagging): bit 3 of an object's ADDRESS distinguishes young (set)
// from old (clear) space, so every heap object must stay 16-aligned and the
// young-space mmap must leave that bit usable — an invariant to preserve on any
// port, not an x86-ism.
enum {
	SPACE_TAG = 1 << 3,
	NEW_SPACE_TAG = SPACE_TAG,
	OLD_SPACE_TAG = 0,
	HEAP_OBJECT_ALIGN = 16,
};

enum {
	TAG_FREESPACE = 1,
	TAG_MARKED = 1 << 2,
	TAG_FORWARDED = 1 << 3,
	TAG_FINALIZED = 1 << 4,
	TAG_REMEMBERED = 1 << 5,
} ObjectTag;

typedef enum {
	VALUE_INT = 0,      // 00: SmallInteger, 62-bit payload
	VALUE_POINTER = 1,  // 01: heap object (asObject subtracts the tag)
	VALUE_CHAR = 2,     // 10: Character
	VALUE_FLOAT = 3,    // 11: SmallFloat64, immediate double (see tagFloat below)
} ValueType;

typedef uintptr_t Value;
typedef intptr_t SignedValue;

// The whole VM assumes 64-bit tagged Values: tagInt carries 62-bit payloads and
// jit/CodeDescriptors.h packs descriptors into bits 16-63. 32-bit targets are
// out of scope by design.
_Static_assert(sizeof(Value) == 8, "the VM requires a 64-bit Value/pointer size");

typedef struct {
	uint8_t tag;
	uint8_t payloadSize;
	uint8_t varsSize;
	uint8_t isIndexed;
	uint8_t isBytes;
	uint8_t valueType;
	uint16_t size;
} InstanceShape;

typedef struct RawClass {
	OBJECT_HEADER;
	Value superClass;
	Value subClasses;
	Value methodDictionary;
	InstanceShape instanceShape;
	Value instanceVariables;
	Value name;
	Value comment;
	Value category;
	Value classVariables;
} RawClass;
OBJECT_HANDLE(Class);

typedef struct {
	OBJECT_HEADER;
	Value superClass;
	Value subClasses;
	Value methodDictionary;
	InstanceShape instanceShape;
	Value instanceVariables;
	Value instanceClass;
} RawMetaClass;
OBJECT_HANDLE(MetaClass);

typedef struct {
	OBJECT_HEADER;
	uint8_t body[];
} RawObject;
OBJECT_HANDLE(Object);

typedef struct {
	OBJECT_HEADER;
	Value size;
	uint8_t body[];
} RawIndexedObject;
OBJECT_HANDLE(IndexedObject);

typedef struct {
	OBJECT_HEADER;
	Value size;
	Value vars[];
} RawArray;
OBJECT_HANDLE(Array);

typedef struct {
	OBJECT_HEADER;
	Value key;
	Value value;
} RawAssociation;
OBJECT_HANDLE(Association);

typedef struct {
	OBJECT_HEADER;
	double value;
} RawFloat;
OBJECT_HANDLE(Float);

#define COMPUTE_INST_SHAPE_SIZE(aPayloadSize, aVarsSize, aIsIndexed) \
	HEADER_SIZE + ((aIsIndexed) + (aPayloadSize) + (aVarsSize)) * sizeof(Value)
#define DEFINE_INST_SHAPE(aPayloadSize, aVarsSize, aIsIndexed, aIsBytes, aValueType) { \
	.tag = 0, \
	.payloadSize = (aPayloadSize), \
	.varsSize = (aVarsSize), \
	.isIndexed = (aIsIndexed), \
	.isBytes = (aIsBytes), \
	.valueType = (aValueType), \
	.size = COMPUTE_INST_SHAPE_SIZE(aPayloadSize, aVarsSize, aIsIndexed), \
}

static InstanceShape FixedShape = DEFINE_INST_SHAPE(0, 0, 0, 0, 0);
static InstanceShape IndexedShape = DEFINE_INST_SHAPE(0, 0, 1, 0, VALUE_POINTER);
static InstanceShape StringShape = DEFINE_INST_SHAPE(0, 0, 1, 1, VALUE_CHAR);
static InstanceShape BytesShape = DEFINE_INST_SHAPE(0, 0, 1, 1, VALUE_INT);
static InstanceShape CompiledCodeShape = DEFINE_INST_SHAPE(1, 0, 1, 1, VALUE_INT);
static InstanceShape BlockShape = DEFINE_INST_SHAPE(1, 0, 0, 0, 0);
static InstanceShape ContextShape = DEFINE_INST_SHAPE(2, 0, 1, 0, VALUE_POINTER);
static InstanceShape ExceptionHandlerShape = DEFINE_INST_SHAPE(1, 2, 0, 0, VALUE_POINTER);
static InstanceShape FloatShape = DEFINE_INST_SHAPE(1, 0, 0, 0, VALUE_INT);

#define varOffset(type, member) (offsetof(type, member) - 1)

static inline size_t computeInstanceSize(InstanceShape shape, size_t size);
static inline size_t computeObjectSize(Object *object);
static inline size_t computeRawObjectSize(RawObject *object);
static inline size_t objectSize(Object *object);
static inline size_t rawObjectSize(RawObject *object);
static inline Value *getObjectVars(Object *object);
static inline Value *getRawObjectVars(RawObject *object);
static inline Value *getRawObjectVarsFromShape(RawObject *object, InstanceShape shape);
static inline uint8_t *getObjectIndexedVars(Object *object);
static inline uint8_t *getRawObjectIndexedVars(RawObject *object);
static inline uint8_t *getRawObjectIndexedVarsFromShape(RawObject *object, InstanceShape shape);
static inline uintptr_t objectGetHash(Object *object);
static inline _Bool isNewObject(RawObject *object);
static inline _Bool isOldObject(RawObject *object);

static inline intptr_t asCInt(Value value);
static inline char asCChar(Value value);
static inline RawObject *asObject(Value value);

static inline Value tagInt(intptr_t i);
static inline Value tagChar(char ch);
static inline Value tagPtr(void *object);

static inline _Bool valueTypeOf(Value value, ValueType type);


static inline size_t computeInstanceSize(InstanceShape shape, size_t size)
{
	size_t varSize = shape.isBytes ? 1 : sizeof(Value);
	return shape.size + size * varSize;
}


static inline size_t computeObjectSize(Object *object)
{
	return computeRawObjectSize(object->raw);
}


static inline size_t computeRawObjectSize(RawObject *object)
{
	return computeInstanceSize(object->class->instanceShape, rawObjectSize(object));
}


static inline size_t objectSize(Object *object)
{
	return rawObjectSize(object->raw);
}


static inline size_t rawObjectSize(RawObject *object)
{
	return object->class->instanceShape.isIndexed ? ((RawIndexedObject *) object)->size : 0;
}


static inline Value *getObjectVars(Object *object)
{
	return getRawObjectVars(object->raw);
}


static inline Value *getRawObjectVars(RawObject *object)
{
	return getRawObjectVarsFromShape(object, object->class->instanceShape);
}


static inline Value *getRawObjectVarsFromShape(RawObject *object, InstanceShape shape)
{
	return (Value *) (object->body + (shape.isIndexed + shape.payloadSize) * sizeof(Value));
}


static inline uint8_t *getObjectIndexedVars(Object *object)
{
	return getRawObjectIndexedVars(object->raw);
}


static inline uint8_t *getRawObjectIndexedVars(RawObject *object)
{
	return getRawObjectIndexedVarsFromShape(object, object->class->instanceShape);
}


static inline uint8_t *getRawObjectIndexedVarsFromShape(RawObject *object, InstanceShape shape)
{
	return (uint8_t *) (getRawObjectVarsFromShape(object, shape) + shape.varsSize);
}


static inline uintptr_t objectGetHash(Object *object)
{
	return object->raw->hash;
}


static inline double rawFloatValue(RawObject *object)
{
	return ((RawFloat *) object)->value;
}


static inline _Bool isNewObject(RawObject *object)
{
	return ((uintptr_t) object & SPACE_TAG) == NEW_SPACE_TAG;
}


static inline _Bool isOldObject(RawObject *object)
{
	return ((uintptr_t) object & SPACE_TAG) == OLD_SPACE_TAG;
}


static inline intptr_t asCInt(Value value)
{
	ASSERT((value & 3) == VALUE_INT);
	return (SignedValue) value >> 2;
}


static inline char asCChar(Value value)
{
	ASSERT((value & 3) == VALUE_CHAR);
	// Tagged Characters carry the byte as UNSIGNED (see tagChar), so the
	// round-trip is identical on signed- and unsigned-char targets.
	return (char) (unsigned char) (value >> 2);
}


static inline RawObject *asObject(Value value)
{
	ASSERT((value & 3) == VALUE_POINTER);
	return (RawObject *) (value - 1);
}


static inline Value tagInt(intptr_t i)
{
	int64_t max = (int64_t) (UINT64_MAX >> 2);
	int64_t min = -max;
	ASSERT(min <= i && i <= max);
	return i << 2;
}


static inline Value tagChar(char ch)
{
	// Normalize through unsigned char: bytes 128..255 tag to the same Value on
	// every target (a signed char would sign-extend and smear the high bits).
	return ((Value) (unsigned char) ch << 2) + VALUE_CHAR;
}


static inline Value tagPtr(void *object)
{
	return (Value) object + VALUE_POINTER;
}


static inline _Bool valueTypeOf(Value value, ValueType type)
{
	return (value & 3) == type;
}


// ---- SmallFloat64: an immediate double in the tagged Value word ------------
//
// Spur-style rotation encoding with a 62-bit payload. ROL64(bits, 1) lines the
// IEEE-754 double up as [exponent:11 at 63..53 | mantissa:52 at 52..1 | sign at 0];
// subtracting the offset (768 << 53) keeps the full mantissa and drops the two
// top exponent bits, so exactly the doubles with biased exponent 768..1279
// (magnitude in 2^[-255, 256], about 1.7e-77 .. 2.3e77) fit unsigned below
// 2^62. +-0.0 is special-cased to payloads 0 and 1; +-2^-255 with a zero
// mantissa would collide with those payloads and stays boxed, as do
// subnormals, infinities, NaN and out-of-range magnitudes (they underflow or
// overflow the payload range). Decode is the exact inverse: add the offset
// back (payloads 0/1 excepted) and rotate right once. The scheme is proven
// bit-exact by the exhaustive ST_SMALLFLOAT_TEST self-test; keep that green
// when touching any of this.
#define SMALLFLOAT_OFFSET ((uint64_t) 768 << 53)

static inline uint64_t doubleToBits(double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}


static inline double bitsToDouble(uint64_t bits)
{
	double value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}


static inline _Bool smallFloatFits(double value)
{
	uint64_t bits = doubleToBits(value);
	if ((bits & ~((uint64_t) 1 << 63)) == 0) {
		return 1; // +-0.0
	}
	uint64_t payload = ((bits << 1) | (bits >> 63)) - SMALLFLOAT_OFFSET;
	return payload >= 2 && payload < ((uint64_t) 1 << 62);
}


// Encode a double as a tagged immediate. Requires smallFloatFits(value).
static inline Value tagFloat(double value)
{
	uint64_t bits = doubleToBits(value);
	uint64_t payload;
	if ((bits & ~((uint64_t) 1 << 63)) == 0) {
		payload = bits >> 63;
	} else {
		payload = ((bits << 1) | (bits >> 63)) - SMALLFLOAT_OFFSET;
		ASSERT(payload >= 2 && payload < ((uint64_t) 1 << 62));
	}
	return (payload << 2) | VALUE_FLOAT;
}


// Decode a tagged immediate back to its double. Requires the VALUE_FLOAT tag.
static inline double floatValueOf(Value value)
{
	ASSERT((value & 3) == VALUE_FLOAT);
	uint64_t payload = value >> 2;
	uint64_t bits = payload <= 1 ? payload : payload + SMALLFLOAT_OFFSET;
	return bitsToDouble((bits >> 1) | (bits << 63));
}

#endif
