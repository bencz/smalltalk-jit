#ifndef OBJECT_H
#define OBJECT_H

#include "core/Assert.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tagged values
// ---------------------------------------------------------------------------
//
// Two tag bits, all four encodings in use. Carried over unchanged from the
// pre-jit-v2 VM because it was already right: SmallInteger at tag 00 makes the
// two-operand integer guard an `or` followed by a `test`, and SmallFloat64
// already exists as an immediate with a WIDER window than the reference
// prototype's (see the rotation encoding at the bottom of this file).

typedef enum {
	VALUE_INT = 0,      // 00: SmallInteger, 62-bit signed payload
	VALUE_POINTER = 1,  // 01: heap object (asObject subtracts the tag)
	VALUE_CHAR = 2,     // 10: Character
	VALUE_FLOAT = 3,    // 11: SmallFloat64, immediate double (see tagFloat)
} ValueType;

typedef uintptr_t Value;
typedef intptr_t SignedValue;

// The whole VM assumes 64-bit tagged Values: tagInt carries 62-bit payloads and
// the object header packs five fields into one word. 32-bit targets are out of
// scope by design.
_Static_assert(sizeof(Value) == 8, "the VM requires a 64-bit Value/pointer size");

// PORT_ME(addr-tagging): bit 3 of an object's ADDRESS distinguishes young (set)
// from old (clear) space, so every heap object must stay 16-aligned and the
// young-space mapping must leave that bit usable. An invariant of the memory
// model, not an x86-ism (ADR 0003, requirement R9).
enum {
	SPACE_TAG = 1 << 3,
	NEW_SPACE_TAG = SPACE_TAG,
	OLD_SPACE_TAG = 0,
	HEAP_OBJECT_ALIGN = 16,
};

// ---------------------------------------------------------------------------
// Object header: ONE word (ADR 0005)
// ---------------------------------------------------------------------------
//
//   bits  0..21   classIndex     22 bits, index into the class table
//   bits 22..43   identityHash   22 bits
//   bits 44..48   format          5 bits, see ObjectFormat
//   bits 49..56   sizeWords       8 bits, TOTAL object size in 8-byte words,
//                                 INCLUDING this header; SIZE_WORDS_BIG means
//                                 the size sits in a word before the header
//   bits 57..63   gc              7 bits, see ObjectGcBit
//
// THE CLASS IS AN INDEX, NOT A POINTER, and that single choice pays in four
// places at once:
//
//   * an inline-cache guard becomes one 32-bit compare against an immediate,
//     with no load of the class and no second cache line touched;
//   * a class baked into generated code is an immediate that no collection has
//     to relocate, retiring most of the old baked-pointer machinery;
//   * inline-cache cells stop having to be wiped at every collection, because
//     an index does not die with the GC epoch the way an address does. That is
//     what makes the phase-2 type profile cumulative instead of amnesiac;
//   * the header halves, 16 bytes to 8.
//
// `sizeWords` is the TOTAL size, not an element count, and that is deliberate:
// it makes "how big is this object" answerable from the header alone for EVERY
// format, with no access to the class (ADR 0003, requirement R6). The collector
// strides the heap on this field and nothing else. An indexed object still
// needs its ELEMENT count for basicSize, and carries it as the first body word,
// which the collector never reads.
//
// `format` answers the collector's other question, "does this contain
// pointers", with no access to the class either, except for the two MIXED
// formats. That exception is deliberate and small: it covers CompiledMethod,
// CompiledBlock, Block, Context and ExceptionHandler, which genuinely
// interleave raw words with tagged ones. Everything the collector walks in bulk
// (ordinary objects, Arrays, Strings, and the flat value arrays of phase 7)
// answers from the header.

#define OBJ_CLASS_BITS 22
#define OBJ_HASH_BITS 22
#define OBJ_FORMAT_BITS 5
#define OBJ_SIZE_BITS 8

#define OBJ_CLASS_SHIFT 0
#define OBJ_HASH_SHIFT (OBJ_CLASS_SHIFT + OBJ_CLASS_BITS)
#define OBJ_FORMAT_SHIFT (OBJ_HASH_SHIFT + OBJ_HASH_BITS)
#define OBJ_SIZE_SHIFT (OBJ_FORMAT_SHIFT + OBJ_FORMAT_BITS)
#define OBJ_GC_SHIFT (OBJ_SIZE_SHIFT + OBJ_SIZE_BITS)

#define OBJ_CLASS_MASK (((uint64_t) 1 << OBJ_CLASS_BITS) - 1)
#define OBJ_HASH_MASK (((uint64_t) 1 << OBJ_HASH_BITS) - 1)
#define OBJ_FORMAT_MASK (((uint64_t) 1 << OBJ_FORMAT_BITS) - 1)
#define OBJ_SIZE_MASK (((uint64_t) 1 << OBJ_SIZE_BITS) - 1)

_Static_assert(OBJ_GC_SHIFT == 57, "header field layout drifted");

// Largest class index the header can hold. Exhausting it must be a clean error,
// never a silent wrap into another class's identity.
#define CLASS_INDEX_MAX (OBJ_CLASS_MASK - 1)

// sizeWords == SIZE_WORDS_BIG means "too big for this field, derive it".
//
// The first design put the real size in a word BEFORE the header. That was
// wrong, and wrong in the expensive way: it needed a prefix word that the
// allocator never reserved and the heap walk would have had to step over. The
// self-test found it as a size of (size_t) -2 on the symbol table, which is
// 1024 elements and therefore big on its very first allocation.
//
// The fix is that no extra word is needed at all. EVERY indexed format already
// carries its element count in body word 0, so the size follows from the format
// and that count. The rare non-indexed object too large for the field derives
// its size from its class, which is the same exception the two MIXED formats
// already make for finding their pointers.
#define SIZE_WORDS_BIG OBJ_SIZE_MASK
#define SIZE_INLINE_MAX_BYTES ((size_t) (SIZE_WORDS_BIG - 1) * sizeof(uint64_t))

typedef enum {
	// Scan nothing, fixed size. Every body word is raw: BoxedFloat64, and any
	// value class whose fields are all scalars.
	FORMAT_NO_POINTERS = 0,
	// Scan every body word. The common case: named instance variables only.
	FORMAT_POINTERS = 1,
	// Body word 0 is the element count, words 1.. are tagged. Array, and any
	// class with named slots followed by indexed ones (the named slots come
	// first and are pointers too, so scanning does not distinguish them).
	FORMAT_INDEXED_POINTERS = 2,
	// Body word 0 is the BYTE count, bytes follow. Scan nothing.
	// String, Symbol, ByteArray, LargeInteger, CompiledCode bytecodes.
	FORMAT_BYTES = 3,
	// Body word 0 is the element count, raw doubles follow. Scan nothing.
	// This is what makes a flat array of a value class free to collect:
	// FloatArray today, `Vec3 arrayNew:` in phase 7 (requirement R5).
	FORMAT_DOUBLES = 4,
	// `rawWords` unscanned words, then tagged words to the end of the object.
	// Context (thread and frame pointers ahead of a variable run of tagged
	// slots), Block, ExceptionHandler.
	FORMAT_MIXED_POINTERS = 5,
	// `rawWords` unscanned words, then `pointerWords` tagged words, then raw
	// bytes to the end. CompiledMethod and CompiledBlock: a native-code pointer
	// and a header word, then the literal frame and friends, then bytecodes.
	FORMAT_MIXED_BYTES = 6,
	// Not a live object.
	FORMAT_FORWARDED = 7,
	FORMAT_FREE = 8,
} ObjectFormat;

// The two formats that cannot answer "where are my pointers" from the header
// alone. They exist because a handful of kernel classes genuinely interleave
// raw words with tagged ones, and they are rare BY COUNT: everything the
// collector walks in bulk answers from the header.
static inline _Bool formatNeedsClass(ObjectFormat format)
{
	return format == FORMAT_MIXED_POINTERS || format == FORMAT_MIXED_BYTES;
}

typedef enum {
	GC_MARKED = 1 << 0,
	GC_REMEMBERED = 1 << 1,
	GC_FINALIZED = 1 << 2,
	GC_PINNED = 1 << 3,
	GC_WEAK = 1 << 4,
	// Survived one young collection. The whole aging policy: an object with
	// this bit is promoted to the old space instead of copied again, so two
	// survivals is the promotion threshold.
	GC_AGED = 1 << 5,
} ObjectGcBit;

typedef struct RawObject {
	uint64_t header;
	uint8_t body[];
} RawObject;

#define HEADER_SIZE (sizeof(uint64_t))

#define OBJECT_HANDLE(name) \
	typedef union name { \
		Raw##name *raw; \
		uintptr_t ptr; \
	} name

// Every heap struct starts with this. Field access is by name everywhere, so
// this declaration is the only encoding of the layout.
#define OBJECT_HEADER uint64_t header

// The generic handle: a scoped, GC-updated reference to any object. Declared
// here because nearly every header needs the type.
OBJECT_HANDLE(Object);


static inline uint32_t rawObjectClassIndex(RawObject *object)
{
	return (uint32_t) ((object->header >> OBJ_CLASS_SHIFT) & OBJ_CLASS_MASK);
}


static inline void rawObjectSetClassIndex(RawObject *object, uint32_t index)
{
	ASSERT(index <= CLASS_INDEX_MAX);
	object->header = (object->header & ~(OBJ_CLASS_MASK << OBJ_CLASS_SHIFT))
		| ((uint64_t) index << OBJ_CLASS_SHIFT);
}


static inline uint32_t rawObjectHash(RawObject *object)
{
	return (uint32_t) ((object->header >> OBJ_HASH_SHIFT) & OBJ_HASH_MASK);
}


static inline ObjectFormat rawObjectFormat(RawObject *object)
{
	return (ObjectFormat) ((object->header >> OBJ_FORMAT_SHIFT) & OBJ_FORMAT_MASK);
}


static inline void rawObjectSetFormat(RawObject *object, ObjectFormat format)
{
	object->header = (object->header & ~(OBJ_FORMAT_MASK << OBJ_FORMAT_SHIFT))
		| ((uint64_t) format << OBJ_FORMAT_SHIFT);
}


static inline uint8_t rawObjectGcBits(RawObject *object)
{
	return (uint8_t) (object->header >> OBJ_GC_SHIFT);
}


static inline void rawObjectSetGcBit(RawObject *object, ObjectGcBit bit)
{
	object->header |= (uint64_t) bit << OBJ_GC_SHIFT;
}


static inline void rawObjectClearGcBit(RawObject *object, ObjectGcBit bit)
{
	object->header &= ~((uint64_t) bit << OBJ_GC_SHIFT);
}


static inline _Bool rawObjectHasGcBit(RawObject *object, ObjectGcBit bit)
{
	return (rawObjectGcBits(object) & bit) != 0;
}


static inline _Bool rawObjectIsBig(RawObject *object)
{
	return ((object->header >> OBJ_SIZE_SHIFT) & OBJ_SIZE_MASK) == SIZE_WORDS_BIG;
}


// The size field as stored, WITHOUT interpreting the BIG sentinel. Only
// memory/ObjectWalk.h should call this; everything else goes through
// objectSizeInBytes, which knows how to derive a big object's size.
static inline size_t rawObjectInlineSizeWords(RawObject *object)
{
	return (size_t) ((object->header >> OBJ_SIZE_SHIFT) & OBJ_SIZE_MASK);
}


static inline uint64_t makeObjectHeader(uint32_t classIndex, uint32_t hash,
	ObjectFormat format, size_t sizeWords)
{
	ASSERT(classIndex <= CLASS_INDEX_MAX);
	ASSERT(sizeWords >= 1);
	// A size that does not fit stores the sentinel; ObjectWalk derives the real
	// one from the element count or the class.
	uint64_t encoded = sizeWords >= SIZE_WORDS_BIG ? SIZE_WORDS_BIG : sizeWords;
	return ((uint64_t) classIndex << OBJ_CLASS_SHIFT)
		| (((uint64_t) hash & OBJ_HASH_MASK) << OBJ_HASH_SHIFT)
		| ((uint64_t) format << OBJ_FORMAT_SHIFT)
		| (encoded << OBJ_SIZE_SHIFT);
}


// Does this format put any tagged pointer in the body? The collector's other
// question, answered with no memory access beyond the header itself.
static inline _Bool formatHasPointers(ObjectFormat format)
{
	return format == FORMAT_POINTERS || format == FORMAT_INDEXED_POINTERS
		|| format == FORMAT_MIXED_POINTERS || format == FORMAT_MIXED_BYTES;
}


// Element count of an indexed object: the first body word. Never read by the
// collector, which strides on sizeWords.
static inline size_t rawObjectElementCount(RawObject *object)
{
	ASSERT(rawObjectFormat(object) == FORMAT_INDEXED_POINTERS
		|| rawObjectFormat(object) == FORMAT_BYTES
		|| rawObjectFormat(object) == FORMAT_DOUBLES
		|| rawObjectFormat(object) == FORMAT_MIXED_BYTES);
	// Always body word 0, in every indexed format including MIXED_BYTES, where
	// it is simply the first of the raw words.
	return (size_t) *(uint64_t *) object->body;
}


static inline void rawObjectSetElementCount(RawObject *object, size_t count)
{
	*(uint64_t *) object->body = count;
}


static inline Value *rawObjectIndexedPointers(RawObject *object)
{
	ASSERT(rawObjectFormat(object) == FORMAT_INDEXED_POINTERS);
	return (Value *) object->body + 1;
}


static inline uint8_t *rawObjectBytes(RawObject *object)
{
	ASSERT(rawObjectFormat(object) == FORMAT_BYTES);
	return object->body + sizeof(uint64_t);
}


static inline double *rawObjectDoubles(RawObject *object)
{
	ASSERT(rawObjectFormat(object) == FORMAT_DOUBLES);
	return (double *) (object->body + sizeof(uint64_t));
}


// ---------------------------------------------------------------------------
// Instance shape: how a class STAMPS a header onto a new instance
// ---------------------------------------------------------------------------
//
// The format lives in the object so the collector never needs the class; this
// struct is the class's template for producing it. `rawWords` and
// `pointerWords` are meaningful only for FORMAT_MIXED, `fixedSlots` only for
// the formats with named instance variables.

typedef struct {
	uint8_t format;       // ObjectFormat
	uint8_t rawWords;     // FORMAT_MIXED: leading unscanned body words
	uint8_t pointerWords; // FORMAT_MIXED: tagged words after the raw ones
	uint16_t fixedSlots;  // named instance variables
} InstanceShape;

#define DEFINE_SHAPE(aFormat, aRawWords, aPointerWords, aFixedSlots) \
	{ .format = (aFormat), .rawWords = (aRawWords), \
	  .pointerWords = (aPointerWords), .fixedSlots = (aFixedSlots) }

// ---------------------------------------------------------------------------
// Kernel object layouts
// ---------------------------------------------------------------------------
//
// Every one starts with OBJECT_HEADER, so `offsetof` is the single encoding of
// the layout and nothing recomputes field positions by hand.

typedef struct RawClass {
	OBJECT_HEADER;
	Value superClass;
	Value subClasses;
	Value methodDictionary;
	Value instanceVariables;
	Value name;
	Value comment;
	Value category;
	Value classVariables;
	Value namespace;        // home Namespace; nil or Core for kernel classes
	// Raw trailer, NOT scanned. FORMAT_MIXED with pointerWords covering the
	// tagged fields above describes this exactly.
	InstanceShape instanceShape; // shape stamped onto instances of this class
	uint32_t classIndex;         // this class's own slot in the class table
} RawClass;
OBJECT_HANDLE(Class);

typedef struct {
	OBJECT_HEADER;
	Value superClass;
	Value subClasses;
	Value methodDictionary;
	Value instanceVariables;
	Value instanceClass;
	InstanceShape instanceShape;
	uint32_t classIndex;
} RawMetaClass;
OBJECT_HANDLE(MetaClass);

typedef struct {
	OBJECT_HEADER;
	uint64_t size;   // element count (the indexed formats' first body word)
	Value vars[];
} RawArray;
OBJECT_HANDLE(Array);

typedef struct {
	OBJECT_HEADER;
	Value key;
	Value value;
} RawAssociation;
OBJECT_HANDLE(Association);

// A package's name universe: `bindings` maps Symbol -> Association exactly like
// the core Smalltalk dictionary, `imports` lists the namespaces of the
// package's DIRECT dependencies in manifest order.
typedef struct {
	OBJECT_HEADER;
	Value name;     // Symbol
	Value bindings; // Dictionary: Symbol -> Association
	Value imports;  // Array of Namespace
} RawNamespace;
OBJECT_HANDLE(Namespace);

typedef struct {
	OBJECT_HEADER;
	double value;
} RawFloat;
OBJECT_HANDLE(Float);

// Offset of a tagged field, biased by the pointer tag so generated code can
// address it straight off a tagged register.
#define varOffset(type, member) (offsetof(type, member) - 1)


// A forwarded object keeps its target in the first body word. Only the
// collector writes this, and only during a collection.
static inline RawObject *rawObjectForwardTarget(RawObject *object)
{
	ASSERT(rawObjectFormat(object) == FORMAT_FORWARDED);
	return *(RawObject **) object->body;
}


static inline void rawObjectSetForward(RawObject *object, RawObject *target)
{
	// The size field is left ALONE: a from-space walk still has to stride past
	// this corpse, and sizeWords is the only thing it strides on.
	rawObjectSetFormat(object, FORMAT_FORWARDED);
	*(RawObject **) object->body = target;
}


static inline _Bool rawObjectIsForwarded(RawObject *object)
{
	return rawObjectFormat(object) == FORMAT_FORWARDED;
}


static inline _Bool isNewObject(RawObject *object)
{
	return ((uintptr_t) object & SPACE_TAG) == NEW_SPACE_TAG;
}


static inline _Bool isOldObject(RawObject *object)
{
	return ((uintptr_t) object & SPACE_TAG) == OLD_SPACE_TAG;
}


// ---------------------------------------------------------------------------
// Tagging
// ---------------------------------------------------------------------------

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
	// The payload is a SIGNED 62-bit field, so the range is [-2^61, 2^61-1] and
	// it is ASYMMETRIC. These constants agree with SmallInteger class
	// maxVal/minVal and with the demotion threshold in LargeInteger>>normalize.
	int64_t max = ((int64_t) 1 << 61) - 1;
	int64_t min = -((int64_t) 1 << 61);
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
// Spur-style rotation encoding with a 62-bit payload, unchanged from the
// pre-jit-v2 VM. ROL64(bits, 1) lines the IEEE-754 double up as
// [exponent:11 at 63..53 | mantissa:52 at 52..1 | sign at 0]; subtracting the
// offset (768 << 53) keeps the full mantissa and drops the two top exponent
// bits, so exactly the doubles with biased exponent 768..1279 (magnitude in
// 2^[-255, 256], about 1.7e-77 .. 2.3e77) fit unsigned below 2^62. +-0.0 is
// special-cased to payloads 0 and 1; +-2^-255 with a zero mantissa would
// collide with those payloads and stays boxed, as do subnormals, infinities,
// NaN and out-of-range magnitudes. Decode is the exact inverse. The scheme is
// proven bit-exact by the exhaustive ST_SMALLFLOAT_TEST self-test; keep that
// green when touching any of this.
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
