#ifndef RUNTIME_PRIMITIVES_SHARED_H
#define RUNTIME_PRIMITIVES_SHARED_H

// What the per-domain primitive files share.
//
// The implementations live one file per domain in this directory; the TABLE
// that maps a primitive number to one of them lives in runtime/Primitive.c,
// beside the four functions that read it. This header is what lets those be
// separate translation units: the declarations of every implementation, and the
// handful of helpers that more than one domain needs.
//
// ---------------------------------------------------------------------------
// WHY THE IMPLEMENTATIONS ARE EXTERN AND DECLARED HERE
// ---------------------------------------------------------------------------
//
// They used to be `static` in one 1730-line file, which is why the table could
// name them with nothing declared: one translation unit, definitions above the
// table. Splitting the file breaks that, and there were two ways out.
//
// THE ONE NOT TAKEN: keep them static and have runtime/Primitive.c #include the
// domain files, so the whole thing stays one translation unit and nothing has to
// be added to any build. It is cheaper today and it is wrong for two reasons. A
// .c that is only ever #included is not a translation unit, it just looks like
// one, and nothing else in this tree does it -- so the next person to add these
// files to CMakeLists by reflex gets two static copies of every primitive and NO
// diagnostic at all, because static symbols do not collide. And it buys that
// silence in exchange for the one thing this campaign keeps paying for: a build
// list that does not say what it builds.
//
// THE ONE TAKEN: real translation units, extern implementations, and the
// declarations GENERATED FROM runtime/Primitives.def -- the same list that
// builds the enum and the table. Writing them out by hand would be a SECOND list
// of the same fact, and two halves of the system encoding one fact two ways is
// the exact disease behind the shape-word bug, the empty-slot bug and the
// scaffold-shadowing bug. One list, and every failure mode is loud:
//
//   * an implementation the .def names and nobody defines is a LINK error;
//   * a definition whose signature disagrees is a COMPILE error in the file that
//     defines it, because that file includes these declarations;
//   * a domain file missing from the build is the first of those, not silence.
//
// What it costs is the .def having to distinguish an implemented primitive from
// an absent one, since `Value NULL(Value *, uint64_t);` is not a prototype. That
// is PRIMITIVE_ABSENT, and the reasoning is written at its line in the .def.
//
// AND IT COSTS A BUILD-LIST EDIT. Three gate levels link Primitive.c BY HAND
// with no CMake (levels 3, 7 and 8 of scripts/gate.sh), so a new domain file has
// to reach four build lists, not one. Forgetting has already broken this gate
// twice. scripts/gate.sh therefore globs this directory instead of listing it;
// CMakeLists.txt still lists the files, because a glob there is not re-evaluated
// when a file appears.

#include "runtime/Primitive.h"
#include "core/Class.h"
#include "core/ClassTable.h"
#include "core/Handle.h"
#include "core/Object.h"
#include "core/Thread.h"
#include "memory/Heap.h"

// Every implementation, declared from the list that also builds the table.
// PRIMITIVE_ABSENT expands to nothing: an absent primitive has no function to
// declare, which is the whole difference between the two macros.
//
// A name that appears twice (primAdd serves both IntAddPrimitive and
// FloatAddPrimitive) declares twice, which C allows.
#define PRIMITIVE(id, name, function) Value function(Value *args, uint64_t argc);
#define PRIMITIVE_ABSENT(id, name)
#include "runtime/Primitives.def"
#undef PRIMITIVE
#undef PRIMITIVE_ABSENT


// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// The SmallInteger payload is a SIGNED 62-bit field, so the range is asymmetric
// and a sum that fits in an intptr_t may still not fit in a SmallInteger. Both
// checks are needed, and testing only the C overflow is the mistake that turns
// a large sum into a small negative one.
#define SMALL_INT_MAX (((intptr_t) 1 << 61) - 1)
#define SMALL_INT_MIN (-((intptr_t) 1 << 61))
#define SMALL_INT_PAYLOAD_BITS 62


static inline _Bool smallIntFits(intptr_t value)
{
	return value >= SMALL_INT_MIN && value <= SMALL_INT_MAX;
}


typedef enum {
	NUM_NOT,   // not a number this file can work with
	NUM_INT,
	NUM_FLOAT,
} NumberKind;

typedef struct {
	NumberKind kind;
	intptr_t asInt;
	double asFloat;
} Number;


// Classify one operand. The two immediate cases are pure tag tests; only a heap
// pointer costs a class-index compare, and only a BoxedFloat64 is accepted
// there. A Fraction, a LargeInteger or anything else answers NUM_NOT, and the
// caller fails, which is exactly how the Smalltalk fallback gets its turn.
static inline Number numberOf(Value value)
{
	Number number = { NUM_NOT, 0, 0.0 };
	switch (value & 3) {
	case VALUE_INT:
		number.kind = NUM_INT;
		number.asInt = asCInt(value);
		number.asFloat = (double) number.asInt;
		return number;

	case VALUE_FLOAT:
		number.kind = NUM_FLOAT;
		number.asFloat = floatValueOf(value);
		return number;

	case VALUE_POINTER: {
		// The heap-float case. Handles.BoxedFloat64 can legitimately be unset in
		// a heap that has not bootstrapped it, and dereferencing it then would
		// be a null read on an arithmetic path, so it is checked rather than
		// assumed.
		if (Handles.BoxedFloat64.raw == NULL) {
			return number;
		}
		RawObject *object = asObject(value);
		if (rawObjectClassIndex(object) != classIndexOf(&Handles.BoxedFloat64)) {
			return number;
		}
		number.kind = NUM_FLOAT;
		number.asFloat = ((RawFloat *) object)->value;
		return number;
	}

	default:
		return number; // Character
	}
}


// A double as a BoxedFloat64 on the heap.
//
// This is the SLOW half of a float result and it is the only reason any
// arithmetic primitive allocates. `Float` is abstract in this kernel exactly as
// it is in Spur: SmallFloat64 holds the doubles whose exponent fits the tagged
// word, BoxedFloat64 holds the rest, and which one an operation answers is not
// the program's business.
static inline Value boxedFloat(double value)
{
	// Before the bootstrap has the class there is nowhere to put it, and failing
	// is right: that window is the built-in kernel, which has no float literals.
	if (Handles.BoxedFloat64.raw == NULL) {
		return PRIMITIVE_FAILED;
	}
	Object *box = newObject(&Handles.BoxedFloat64, 0);
	((RawFloat *) box->raw)->value = value;
	return objectTagged(box);
}


// Answer a double, immediate when it fits and BOXED when it does not.
//
// A MACRO, and it has to be: PRIMITIVE_ALLOCATES takes
// __builtin_return_address(0), which names the enclosing function, so inside a
// helper it would anchor the helper's frame instead of the compiled method's.
//
// THE FAST PATH STILL DOES NOT ALLOCATE, which is the property runtime/
// Primitive.h claims for arithmetic: an ordinary sum lands inside the
// SmallFloat64 window and leaves through one compare and a tag, with no anchor
// and no heap traffic. Only a subnormal, an infinity, a NaN or an overflow
// reaches the second half.
//
// It used to FAIL there instead, on the stated grounds that "the method's own
// code" would build the box. It could not: BoxedFloat64.st declares no methods
// and no constructor, so nothing in Smalltalk can make one. That left
// `1.0e300 + 1.0` with no correct answer available anywhere, which is why this
// moved into C.
#define FLOAT_RESULT(args, expression) \
	do { \
		double floatValue_ = (expression); \
		if (smallFloatFits(floatValue_)) { \
			return tagFloat(floatValue_); \
		} \
		PRIMITIVE_ALLOCATES(args); \
		Value floatBoxed_ = boxedFloat(floatValue_); \
		PRIMITIVE_DONE_ALLOCATING(); \
		return floatBoxed_; \
	} while (0)


static inline Value booleanResult(_Bool value)
{
	return tagPtr(value ? Handles.true_.raw : Handles.false_.raw);
}


// Both operands are SmallIntegers. The test is the one the tag scheme was
// chosen for: OR the two Values and test the low two bits, so one `or` and one
// `test` decide the hot case for every arithmetic primitive here.
static inline _Bool bothSmallIntegers(Value a, Value b)
{
	return ((a | b) & 3) == VALUE_INT;
}


// ---------------------------------------------------------------------------
// Receivers that more than one domain has to recognise
// ---------------------------------------------------------------------------

// Is this receiver a CLASS?
//
// Not "is it an instance of the class-of-classes", which is what this used to
// ask and what stopped being true the moment metaclasses arrived: a class is an
// instance of ITS METACLASS, and a metaclass is an instance of the
// class-of-classes. So the test is one level deeper, and it accepts both, which
// is right: `Array new` and `Array class new` are both sends to a class.
//
// The shape is what decides rather than a name, because that is what the
// allocator is about to use. A non-class receiver fails rather than reading
// instanceShape out of whatever object happened to arrive, which would allocate
// a garbage size.
static inline Class *receiverAsClass(Value receiver)
{
	if (!valueTypeOf(receiver, VALUE_POINTER) || Handles.ClassClass.raw == NULL) {
		return NULL;
	}
	RawObject *object = asObject(receiver);
	uint32_t classOfObject = rawObjectClassIndex(object);
	if (classOfObject == classIndexOf(&Handles.ClassClass)) {
		return (Class *) scopeHandle(object); // a metaclass, or a class before it has one
	}
	RawObject *itsClass = classTableAt(&CurrentThread.heap->classes, classOfObject);
	if (itsClass == NULL
			|| rawObjectClassIndex(itsClass) != classIndexOf(&Handles.ClassClass)) {
		return NULL;
	}
	return (Class *) scopeHandle(object); // an instance of a metaclass: a class
}


// Shared by Block.c, which enters one, and Exceptions.c, which protects one.
static inline _Bool isClosure(Value value)
{
	return valueTypeOf(value, VALUE_POINTER) && Handles.Closure.raw != NULL
		&& rawObjectClassIndex(asObject(value)) == classIndexOf(&Handles.Closure);
}

#endif
