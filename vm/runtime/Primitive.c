#include "runtime/Primitive.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/ClassTable.h"
#include "core/Handle.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "jit/CompiledMethod.h"
#include "runtime/Closure.h"
#include "os/Os.h"
#include "os/OsFile.h"
#include "runtime/String.h"
#include <limits.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// The implementations.
//
// Every one of them obeys two rules, and both are load-bearing rather than
// stylistic:
//
//   1. NO ALLOCATION, except in the two that exist to allocate, and those
//      ANCHOR THE CALLING FRAME first (PRIMITIVE_ALLOCATES). Allocating without
//      anchoring means a collection triggered from inside a primitive walks no
//      compiled frames, and evacuates objects the live method underneath is
//      still holding in its registers. Everything else here answers an
//      immediate or an object the caller already holds.
//   2. FAIL RATHER THAN GUESS. A primitive that cannot answer exactly returns
//      PRIMITIVE_FAILED and lets the method's bytecode handle the general case.
//      Overflow fails. A zero divisor fails. An out-of-range index fails. A
//      receiver of the wrong shape fails. None of them approximate.

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


static _Bool smallIntFits(intptr_t value)
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
static Number numberOf(Value value)
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


// A double result, as an immediate or not at all.
//
// The SmallFloat64 window is wide (magnitude in 2^[-255, 256]), so ordinary
// arithmetic lands inside it; a subnormal, an infinity, a NaN or an overflow
// does not, and boxing one would allocate. Failing hands those to the method's
// own code, which is where the general case belongs anyway.
static Value floatResult(double value)
{
	return smallFloatFits(value) ? tagFloat(value) : PRIMITIVE_FAILED;
}


static Value booleanResult(_Bool value)
{
	return tagPtr(value ? Handles.true_.raw : Handles.false_.raw);
}


// Both operands are SmallIntegers. The test is the one the tag scheme was
// chosen for: OR the two Values and test the low two bits, so one `or` and one
// `test` decide the hot case for every arithmetic primitive here.
static _Bool bothSmallIntegers(Value a, Value b)
{
	return ((a | b) & 3) == VALUE_INT;
}


// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

static Value primAdd(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t sum;
		if (__builtin_add_overflow(asCInt(receiver), asCInt(argument), &sum)
				|| !smallIntFits(sum)) {
			return PRIMITIVE_FAILED; // LargeInteger is the fallback's business
		}
		return tagInt(sum);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat + b.asFloat);
}


static Value primSubtract(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t difference;
		if (__builtin_sub_overflow(asCInt(receiver), asCInt(argument), &difference)
				|| !smallIntFits(difference)) {
			return PRIMITIVE_FAILED;
		}
		return tagInt(difference);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat - b.asFloat);
}


static Value primMultiply(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t product;
		if (__builtin_mul_overflow(asCInt(receiver), asCInt(argument), &product)
				|| !smallIntFits(product)) {
			return PRIMITIVE_FAILED;
		}
		return tagInt(product);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat * b.asFloat);
}


// Integer division answers an integer ONLY when it is exact. 7/2 fails, and the
// Smalltalk fallback answers a Fraction. Answering 3 here would be a wrong
// answer that no test of the primitive itself could catch, because 3 is a
// perfectly plausible SmallInteger.
static Value primDivide(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	if (bothSmallIntegers(receiver, argument)) {
		intptr_t divisor = asCInt(argument);
		if (divisor == 0) {
			return PRIMITIVE_FAILED; // ZeroDivide is raised in Smalltalk
		}
		intptr_t dividend = asCInt(receiver);
		if (dividend % divisor != 0) {
			return PRIMITIVE_FAILED; // a Fraction, which allocates
		}
		intptr_t quotient = dividend / divisor;
		return smallIntFits(quotient) ? tagInt(quotient) : PRIMITIVE_FAILED;
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	if (b.asFloat == 0.0) {
		// The quotient would be an infinity or a NaN, neither of which fits an
		// immediate, so this is the same answer floatResult would give. Said
		// here so the intent is division by zero and not a representation
		// accident.
		return PRIMITIVE_FAILED;
	}
	return floatResult(a.asFloat / b.asFloat);
}


// Floor division and floor modulo, SmallInteger only.
//
// C division truncates toward zero; Smalltalk's // and \\ round toward
// negative infinity, so -7 // 2 is -4 and -7 \\ 2 is 1. The correction below is
// the whole difference, and getting it wrong is a wrong answer for exactly the
// negative operands that tests forget.
//
// Float receivers deliberately FAIL here rather than being handled: doing them
// needs floor() from libm, and the Smalltalk fallback is the designed home for
// the general case. That is a scope decision, not an oversight.
static Value primFloorDivide(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t divisor = asCInt(argument);
	if (divisor == 0) {
		return PRIMITIVE_FAILED;
	}
	intptr_t dividend = asCInt(receiver);
	intptr_t quotient = dividend / divisor;
	if (dividend % divisor != 0 && ((dividend < 0) != (divisor < 0))) {
		quotient--;
	}
	return smallIntFits(quotient) ? tagInt(quotient) : PRIMITIVE_FAILED;
}


static Value primFloorModulo(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t divisor = asCInt(argument);
	if (divisor == 0) {
		return PRIMITIVE_FAILED;
	}
	intptr_t remainder = asCInt(receiver) % divisor;
	if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
		remainder += divisor;
	}
	// |remainder| < |divisor| <= 2^61, so it always fits.
	return tagInt(remainder);
}


// quo: and rem: TRUNCATE toward zero, which is what C division already does, so
// they are the pair that needs no correction. // and \\ are the ones that round
// toward negative infinity; keeping both pairs is not redundancy, it is the
// difference between -7 quo: 2 = -3 and -7 // 2 = -4.
static Value primQuotient(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument) || asCInt(argument) == 0) {
		return PRIMITIVE_FAILED;
	}
	intptr_t quotient = asCInt(receiver) / asCInt(argument);
	return smallIntFits(quotient) ? tagInt(quotient) : PRIMITIVE_FAILED;
}


static Value primRemainder(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument) || asCInt(argument) == 0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt(asCInt(receiver) % asCInt(argument));
}


static Value primNegated(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	// minVal has no positive counterpart: the payload range is asymmetric, so
	// negating it overflows and the LargeInteger fallback takes over.
	intptr_t negated = -asCInt(receiver);
	return smallIntFits(negated) ? tagInt(negated) : PRIMITIVE_FAILED;
}


static Value primAsFloat(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	return floatResult((double) asCInt(receiver));
}


// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------
//
// One shape for all six. The integer pair compares payloads directly; a mixed
// pair goes through double, which loses precision above 2^53 and can call two
// distinct large integers equal. Accepted for now and written down rather than
// discovered: the exact mixed comparison belongs with the numeric tower, which
// is fallback code.

typedef enum {
	CMP_LESS,
	CMP_GREATER,
	CMP_LESS_EQUAL,
	CMP_GREATER_EQUAL,
	CMP_EQUAL,
	CMP_NOT_EQUAL,
} Comparison;


static Value compareNumbers(Value *args, uint64_t argc, Comparison how)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);

	_Bool answer;
	if (bothSmallIntegers(receiver, argument)) {
		intptr_t a = asCInt(receiver);
		intptr_t b = asCInt(argument);
		switch (how) {
		case CMP_LESS: answer = a < b; break;
		case CMP_GREATER: answer = a > b; break;
		case CMP_LESS_EQUAL: answer = a <= b; break;
		case CMP_GREATER_EQUAL: answer = a >= b; break;
		case CMP_EQUAL: answer = a == b; break;
		default: answer = a != b; break;
		}
		return booleanResult(answer);
	}

	Number a = numberOf(receiver);
	Number b = numberOf(argument);
	if (a.kind == NUM_NOT || b.kind == NUM_NOT) {
		// `3 = aString` must answer false rather than fail, but that is
		// Object>>= 's job: this primitive is installed on the numeric classes
		// and its contract is "compare two numbers". A non-number argument is
		// precisely the case the fallback exists for.
		return PRIMITIVE_FAILED;
	}
	switch (how) {
	case CMP_LESS: answer = a.asFloat < b.asFloat; break;
	case CMP_GREATER: answer = a.asFloat > b.asFloat; break;
	case CMP_LESS_EQUAL: answer = a.asFloat <= b.asFloat; break;
	case CMP_GREATER_EQUAL: answer = a.asFloat >= b.asFloat; break;
	case CMP_EQUAL: answer = a.asFloat == b.asFloat; break;
	default: answer = a.asFloat != b.asFloat; break;
	}
	return booleanResult(answer);
}


// Only `<` and `=` are primitives, because only those two are what the kernel
// declares: Magnitude derives >, <= and >= from < in Smalltalk. The other four
// comparison shapes stay reachable through compareNumbers so that promoting one
// to a primitive later is a line in Primitives.def and not a rewrite.
static Value primLess(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_LESS); }
static Value primNumericEqual(Value *a, uint64_t n) { return compareNumbers(a, n, CMP_EQUAL); }


// ---------------------------------------------------------------------------
// Bit operations
// ---------------------------------------------------------------------------

typedef enum { BIT_AND, BIT_OR, BIT_XOR } BitOperation;


static Value bitOperation(Value *args, uint64_t argc, BitOperation how)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	// The tag is 00 for both, so the operation applies to the tagged Values
	// directly and the result is already tagged. That is true of and, or and
	// xor, and it is NOT true of addition, which is why only these three take
	// the shortcut.
	switch (how) {
	case BIT_AND: return receiver & argument;
	case BIT_OR: return receiver | argument;
	default: return receiver ^ argument;
	}
}


static Value primBitAnd(Value *a, uint64_t n) { return bitOperation(a, n, BIT_AND); }
static Value primBitOr(Value *a, uint64_t n) { return bitOperation(a, n, BIT_OR); }
static Value primBitXor(Value *a, uint64_t n) { return bitOperation(a, n, BIT_XOR); }


// Positive shifts left, negative shifts right, and a left shift that would
// leave the SmallInteger range fails rather than truncating.
static Value primBitShift(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value argument = primitiveArgument(args, 0);
	if (!bothSmallIntegers(receiver, argument)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t value = asCInt(receiver);
	intptr_t shift = asCInt(argument);

	if (shift >= 0) {
		if (shift >= SMALL_INT_PAYLOAD_BITS) {
			return value == 0 ? tagInt(0) : PRIMITIVE_FAILED;
		}
		// Shift as UNSIGNED: left-shifting a negative signed value is undefined
		// behaviour, and the round trip below is what detects the bits lost.
		intptr_t shifted = (intptr_t) ((uintptr_t) value << shift);
		if ((shifted >> shift) != value || !smallIntFits(shifted)) {
			return PRIMITIVE_FAILED;
		}
		return tagInt(shifted);
	}

	// An arithmetic right shift, saturating: shifting a 62-bit value right by
	// more than its width leaves 0 or -1, and letting the shift count reach the
	// register width would be undefined behaviour instead.
	uintptr_t amount = (uintptr_t) -shift;
	if (amount >= SMALL_INT_PAYLOAD_BITS) {
		return tagInt(value < 0 ? -1 : 0);
	}
	return tagInt(value >> amount);
}


// ---------------------------------------------------------------------------
// Identity and reflection
// ---------------------------------------------------------------------------

static Value primIdentical(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	// Identity is Value equality, and that is exactly right for immediates too:
	// two equal SmallIntegers are the same object because the value IS the
	// object, while two BoxedFloat64s holding the same double are not.
	return booleanResult(primitiveReceiver(args) == primitiveArgument(args, 0));
}


static Value primNotIdentical(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(primitiveReceiver(args) != primitiveArgument(args, 0));
}


static Value primClass(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// classOf is the one place that knows immediates have classes too, so this
	// works for a SmallInteger as well as for a heap object.
	return tagPtr(classOf(primitiveReceiver(args)));
}


static Value primIdentityHash(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	switch (receiver & 3) {
	case VALUE_INT:
		return receiver; // a SmallInteger hashes as itself
	case VALUE_CHAR:
		return tagInt((intptr_t) (receiver >> 2));
	case VALUE_POINTER:
		return tagInt((intptr_t) rawObjectHash(asObject(receiver)));
	default:
		// A Float's hash has to agree with numeric equality across the whole
		// tower, which is a Smalltalk-side decision rather than a header read.
		return PRIMITIVE_FAILED;
	}
}


// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------
//
// The only primitives in this file that touch the heap, and therefore the only
// ones that anchor the calling frame. Everything above answers an immediate or
// an object the caller already holds.

// The receiver has to be a class before its shape can be stamped onto anything.
// A non-class receiver fails rather than reading instanceShape out of whatever
// object happened to arrive, which would allocate a garbage size.
// Is this receiver a CLASS?
//
// Not "is it an instance of the class-of-classes", which is what this used to
// ask and what stopped being true the moment metaclasses arrived: a class is an
// instance of ITS METACLASS, and a metaclass is an instance of the
// class-of-classes. So the test is one level deeper, and it accepts both, which
// is right: `Array new` and `Array class new` are both sends to a class.
//
// The shape is what decides rather than a name, because that is what the
// allocator is about to use.
static Class *receiverAsClass(Value receiver)
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


static Value primBasicNew(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = allocateInstance(args, 0);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


static Value primBasicNewSized(Value *args, uint64_t argc)
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


// ---------------------------------------------------------------------------
// Entering a block
// ---------------------------------------------------------------------------
//
// `aBlock value` is a SEND, exactly like everything else, so the site carries an
// inline cache and the optimizer can see which block actually runs there. What
// the primitive does is enter the closure's own compiled code with the CLOSURE
// as receiver: register 0 of a block's frame is the closure itself, which is how
// GETUP reaches the captured values with one load.

static _Bool isClosure(Value value)
{
	return valueTypeOf(value, VALUE_POINTER) && Handles.Closure.raw != NULL
		&& rawObjectClassIndex(asObject(value)) == classIndexOf(&Handles.Closure);
}


static Value enterClosure(Value *args, uint64_t argc)
{
	Value receiver = primitiveReceiver(args);
	if (!isClosure(receiver)) {
		return PRIMITIVE_FAILED;
	}
	RawClosure *closure = (RawClosure *) asObject(receiver);
	RawCompiledMethod *method = (RawCompiledMethod *) asObject(closure->method);
	if (method->unit->argumentCount != argc) {
		return PRIMITIVE_FAILED; // wrong number of arguments for this block
	}
	if (method->native == NULL) {
		Opcode unsupported;
		method->native = jitCompile(method->unit, &unsupported);
		if (method->native == NULL) {
			return PRIMITIVE_FAILED;
		}
	}
	// Re-read the receiver: compiling allocates nothing on the heap today, but
	// entering the block certainly does, and the value handed to jitCall has to
	// be the current one.
	receiver = primitiveReceiver(args);
	switch (argc) {
	case 0:
		return jitCall0(method->native, receiver);
	case 1:
		return jitCall1(method->native, receiver, primitiveArgument(args, 0));
	default:
		return jitCall2(method->native, receiver, primitiveArgument(args, 0),
			primitiveArgument(args, 1));
	}
}


// Anchored, because the block's own body allocates as freely as any method.
static Value primClosureValue(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 0);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


static Value primClosureValue1(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 1);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


static Value primClosureValue2(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 2);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// Float mathematics
// ---------------------------------------------------------------------------
//
// Every one of these was DECLARED with an empty body, so before this each one
// answered its receiver and a program that took a square root carried on with
// the number it started from. They are batched because they are the same three
// lines each: classify the receiver, call libm, fail if the result does not fit
// the immediate window so the kernel's own code can box it.
//
// FAILING ON A NON-FLOAT RECEIVER IS THE POINT of numberOf answering NUM_NOT:
// `4 sqrt` has an Integer receiver and belongs to Number's Smalltalk code, not
// here.

typedef double (*UnaryMath)(double);

static Value floatUnary(Value *args, uint64_t argc, UnaryMath fn)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	if (self.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(fn(self.asFloat));
}

#define FLOAT_UNARY(name, fn) \
	static Value name(Value *args, uint64_t argc) \
	{ \
		return floatUnary(args, argc, fn); \
	}

FLOAT_UNARY(primFloatSqrt, sqrt)
FLOAT_UNARY(primFloatSin, sin)
FLOAT_UNARY(primFloatCos, cos)
FLOAT_UNARY(primFloatTan, tan)
FLOAT_UNARY(primFloatArcSin, asin)
FLOAT_UNARY(primFloatArcCos, acos)
FLOAT_UNARY(primFloatArcTan, atan)
FLOAT_UNARY(primFloatExp, exp)
FLOAT_UNARY(primFloatLn, log)


static Value primFloatArcTan2(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	Number other = numberOf(primitiveArgument(args, 0));
	if (self.kind == NUM_NOT || other.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(atan2(self.asFloat, other.asFloat));
}


// A whole double as a SmallInteger, or failure.
//
// The window is the tagged one and NOT the double's: a finite double of
// magnitude 2^53 or more is already integral and the kernel answers it by a
// different route, so overflowing here is an ordinary fall-through and not an
// error. Written as a double comparison rather than a cast, because casting a
// double outside the integer range is undefined behaviour in C and would be a
// wrong answer rather than a failure.
static Value integerResult(double value)
{
	if (!(value >= -4611686018427387904.0 && value <= 4611686018427387903.0)) {
		return PRIMITIVE_FAILED; // NaN and infinity land here too, by !(...)
	}
	return tagInt((intptr_t) value);
}


#define FLOAT_TO_INTEGER(name, fn) \
	static Value name(Value *args, uint64_t argc) \
	{ \
		if (argc != 0) { \
			return PRIMITIVE_FAILED; \
		} \
		Number self = numberOf(primitiveReceiver(args)); \
		if (self.kind == NUM_NOT) { \
			return PRIMITIVE_FAILED; \
		} \
		return integerResult(fn(self.asFloat)); \
	}

FLOAT_TO_INTEGER(primFloatFloor, floor)
FLOAT_TO_INTEGER(primFloatCeiling, ceil)
FLOAT_TO_INTEGER(primFloatTruncated, trunc)
FLOAT_TO_INTEGER(primFloatRounded, nearbyint)


// Base-2 exponent: 1.0 answers 0, 0.5 answers -1. Zero, infinity and NaN have
// none, and the kernel raises for them, so this fails on all three.
static Value primFloatExponent(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	if (self.kind == NUM_NOT || !isfinite(self.asFloat) || self.asFloat == 0.0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) ilogb(self.asFloat));
}


// self * 2^n, EXACT: ldexp touches the exponent and leaves the mantissa alone,
// which is what `asExactedFraction` depends on to recover the mantissa bits.
static Value primFloatTimesTwoPower(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	Value power = primitiveArgument(args, 0);
	if (self.kind == NUM_NOT || !valueTypeOf(power, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t n = asCInt(power);
	if (n < INT_MIN || n > INT_MAX) {
		return PRIMITIVE_FAILED;
	}
	return floatResult(ldexp(self.asFloat, (int) n));
}


// A Float as the SHORTEST decimal that reads back as the same double.
//
// Shortest-round-trip and not a fixed precision, because both fixed choices are
// wrong in a way a user sees: `%.17g` prints 3.14 as 3.1400000000000001, and
// anything shorter stops round-tripping, so `x printString asNumber = x` becomes
// false for ordinary values. The loop is the standard way to get it without a
// Grisu implementation: ask for one significant digit, read it back, and widen
// until it matches. Seventeen always matches, so it terminates.
//
// The trailing `.0` is Smalltalk's, not C's: a Float that happens to be integral
// still prints as a Float, or `1500.0 printString` would answer '1500' and the
// value would read back as an Integer.
static Value primFloatAsString(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Number self = numberOf(primitiveReceiver(args));
	if (self.kind == NUM_NOT) {
		return PRIMITIVE_FAILED;
	}
	double value = self.asFloat;

	char buffer[64];
	if (isnan(value)) {
		snprintf(buffer, sizeof buffer, "nan");
	} else if (isinf(value)) {
		snprintf(buffer, sizeof buffer, value < 0 ? "-inf" : "inf");
	} else {
		// `%e` and not `%g` to FIND the digit count, because %g also decides the
		// notation, and it decides it by a rule nobody here wants: at two
		// significant digits it renders 1500.0 as "1.5e+03", which round-trips
		// and is the wrong answer for `1500.0 printString`.
		int digits = 17;
		for (int d = 1; d <= 17; d++) {
			snprintf(buffer, sizeof buffer, "%.*e", d - 1, value);
			if (strtod(buffer, NULL) == value) {
				digits = d;
				break;
			}
		}
		// The notation is then chosen SEPARATELY, on the decimal exponent that
		// %e just reported. Plain for everything a reader would write plainly,
		// scientific only where plain would be a wall of zeros.
		const char *marker = strchr(buffer, 'e');
		long exponent10 = marker == NULL ? 0 : strtol(marker + 1, NULL, 10);
		if (exponent10 >= -5 && exponent10 <= 15) {
			int decimals = (int) (digits - 1 - exponent10);
			snprintf(buffer, sizeof buffer, "%.*f", decimals < 0 ? 0 : decimals,
				value);
		}
		// A bare digit string has to read back as a Float, so it gets ".0"; a
		// scientific mantissa with no point gets one before the exponent.
		if (strpbrk(buffer, ".n") == NULL) {
			char *exponent = strpbrk(buffer, "eE");
			if (exponent == NULL) {
				size_t length = strlen(buffer);
				if (length + 3 <= sizeof buffer) {
					memcpy(buffer + length, ".0", 3);
				}
			} else {
				char tail[64];
				snprintf(tail, sizeof tail, "%s", exponent);
				size_t head = (size_t) (exponent - buffer);
				if (head + 2 + strlen(tail) + 1 <= sizeof buffer) {
					memcpy(buffer + head, ".0", 2);
					memcpy(buffer + head + 2, tail, strlen(tail) + 1);
				}
			}
		}
	}

	// Allocates, so the caller's compiled frames are anchored first.
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(buffer));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// Time, and the heap's own numbers
// ---------------------------------------------------------------------------

static Value primMonotonicNanos(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// A monotonic reading is 60-odd bits after a few weeks of uptime, so it can
	// legitimately outgrow the tagged window; failing is right, and the kernel's
	// own code is where a LargeInteger would come from.
	int64_t nanos = osMonotonicNanos();
	return smallIntFits(nanos) ? tagInt((intptr_t) nanos) : PRIMITIVE_FAILED;
}


static Value primCurrentMicroTime(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	int64_t micros = osCurrentMicroTime();
	return smallIntFits(micros) ? tagInt((intptr_t) micros) : PRIMITIVE_FAILED;
}


static Value primCollectGarbage(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// A COLLECTION FROM SMALLTALK, so the caller's compiled frames underneath
	// have to be reachable. PRIMITIVE_ALLOCATES is what anchors them, and it is
	// needed here for the same reason an allocating primitive needs it even
	// though nothing is allocated: the collector walks from the anchor.
	PRIMITIVE_ALLOCATES(args);
	collectGarbage(CurrentThread.heap);
	PRIMITIVE_DONE_ALLOCATING();
	return primitiveReceiver(args);
}


static Value primPrintHeap(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	printHeap(CurrentThread.heap);
	return primitiveReceiver(args);
}


// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// A String's hash is over its CONTENTS, and that is what makes it agree with an
// interned Symbol's identity hash by construction (runtime/String.c, asSymbol).
static Value primStringHash(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)
			|| rawObjectFormat(asObject(receiver)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	return tagInt(stringHash((RawString *) asObject(receiver)));
}


static Value primStringAsSymbol(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)
			|| rawObjectFormat(asObject(receiver)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	// Interning ALLOCATES when the symbol is new, and it can grow the symbol
	// table, so the caller's frames have to be anchored first.
	PRIMITIVE_ALLOCATES(args);
	String *symbol = asSymbol((String *) scopeHandle(asObject(primitiveReceiver(args))));
	Value answer = objectTagged(symbol);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------
//
// Three primitives and one chain (jit/Jit.h). The kernel writes the whole
// PROTOCOL in Smalltalk -- `signal`, `return:`, `resume:`, `retry`, `pass`,
// `ensure:` -- and what is here is only what Smalltalk cannot express: taking a
// jump destination, finding a handler across frames the language cannot see, and
// running a cleanup on the way out.
//
// THE SETJMP IS TAKEN IN THE PRIMITIVE'S OWN FRAME, and it has to be: a jump
// resumes in the frame that took the destination, so pushing it one call deeper
// would resume in a frame that no longer exists. That is the same rule
// ENTER_COMPILED follows for a non-local return, and it is why these three are
// written out here instead of being one helper each.

// Block>>basicOn: anExceptionClass do: aHandlerBlock
static Value primBlockOnException(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	Value protectedBlock = primitiveReceiver(args);
	if (!isClosure(protectedBlock)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);

	UnwindRecord record;
	unwindPushHandler(&record, primitiveArgument(args, 0),
		primitiveArgument(args, 1));
	Value answer;
	if (setjmp(record.destination) != 0) {
		// A handler chose to unwind to here. Everything between the signal and
		// this frame is gone, and unwindAnswer is what puts back the bookkeeping
		// those frames would have restored on the way out.
		answer = unwindAnswer(&record);
	} else {
		answer = jitSendUnary(protectedBlock, "value", NULL);
		unwindPop(&record);
	}
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Exception>>basicSignal, and HandlerEscape>>signal, which is the same
// mechanism used by the machinery that implements resume:/return:/retry.
static Value primExceptionSignal(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	// PRIMITIVE_FAILED when nothing handled it, which is not an error here: the
	// method's own `^self defaultAction` is the general case, exactly as the
	// fall-through works for every other primitive.
	Value answer = jitSignalException(primitiveReceiver(args));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Block>>valueUnwindProtected: aCleanupBlock
//
// On the NORMAL path this answers and the kernel's `ensure:` runs the cleanup
// itself, one line later. On an UNWIND the unwinder runs it, and this frame is
// never returned to. Exactly once either way, which is the whole contract.
static Value primBlockUnwindProtected(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value protectedBlock = primitiveReceiver(args);
	if (!isClosure(protectedBlock)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	UnwindRecord record;
	unwindPushCleanup(&record, primitiveArgument(args, 0));
	Value answer = jitSendUnary(protectedBlock, "value", NULL);
	unwindPop(&record);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// The packed shape word of a class, which Smalltalk cannot read as a field:
// jit-v2 keeps it in the class's RAW TRAILER so the collector never walks it
// (ADR 0005), and that is exactly the kind of thing a primitive is for.
static Value primInstanceShape(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Class *class = receiverAsClass(primitiveReceiver(args));
	if (class == NULL) {
		return PRIMITIVE_FAILED;
	}
	// PACKED FIELD BY FIELD, and not by memcpy of the struct.
	//
	// InstanceShape has PADDING between pointerWords and fixedSlots, and the C
	// standard leaves padding bytes indeterminate. A memcpy therefore hands
	// Smalltalk a word with garbage bits in it, and the number a class answers
	// for its own shape could differ between two builds, or two runs, for
	// reasons no reader could see.
	//
	// The layout below is the CONTRACT with Behavior in packages/Core, which
	// decodes it. It is stated here and repeated there, and nowhere else:
	//
	//   bits  0..7   format (ObjectFormat)
	//   bits  8..15  rawWords
	//   bits 16..23  pointerWords
	//   bits 24..39  fixedSlots
	InstanceShape shape = class->raw->instanceShape;
	uint64_t packed = (uint64_t) shape.format
		| ((uint64_t) shape.rawWords << 8)
		| ((uint64_t) shape.pointerWords << 16)
		| ((uint64_t) shape.fixedSlots << 24);
	return tagInt((intptr_t) packed);
}


// The digits of an integer, as a String.
//
// It ALLOCATES, so it anchors the calling frame first. The Smalltalk fallback
// beside it in the kernel is the definition of what this must agree with, and it
// stays there: this answers only what it can answer exactly, which is a
// SmallInteger in a base from 2 to 36.
static Value primIntAsStringBase(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value baseValue = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_INT) || !valueTypeOf(baseValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t base = asCInt(baseValue);
	if (base < 2 || base > 36) {
		return PRIMITIVE_FAILED; // the fallback signals; guessing here would not
	}

	// Built BACKWARDS into a C buffer first, so the String is allocated once at
	// its exact size. 64 binary digits plus a sign is the widest a SmallInteger
	// can be.
	char digits[72];
	size_t length = 0;
	intptr_t value = asCInt(receiver);
	_Bool negative = value < 0;
	// Through uintptr_t, because negating the most negative value overflows.
	uintptr_t magnitude = negative
		? (uintptr_t) 0 - (uintptr_t) value : (uintptr_t) value;
	if (magnitude == 0) {
		digits[length++] = '0';
	}
	while (magnitude > 0) {
		unsigned digit = (unsigned) (magnitude % (uintptr_t) base);
		digits[length++] = (char) (digit < 10 ? '0' + digit : 'A' + digit - 10);
		magnitude /= (uintptr_t) base;
	}
	if (negative) {
		digits[length++] = '-';
	}

	PRIMITIVE_ALLOCATES(args);
	String *string = newString(length);
	for (size_t i = 0; i < length; i++) {
		string->raw->contents[i] = digits[length - 1 - i];
	}
	Value answer = objectTagged(string);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// Streams: the bytes actually leaving the process
// ---------------------------------------------------------------------------
//
// These are the bottom of the kernel's IO stack. `Transcript` is an
// ExternalStream on descriptor 1, `printNl` fills its buffer, and the buffer
// arrives here. Everything above them is Smalltalk.
//
// They are CLASS-SIDE methods, so the receiver is the class and the descriptor
// is an ordinary argument. None of them allocates.
//
// A short write is not an error and not silently accepted either: the count
// WRITTEN is answered, and the Smalltalk side loops. That is the contract a
// caller can act on; answering "done" after a partial write is how data goes
// missing without anybody noticing.

static _Bool streamDescriptorOf(Value value, int *descriptor)
{
	if (!valueTypeOf(value, VALUE_INT)) {
		return 0;
	}
	intptr_t number = asCInt(value);
	if (number < 0 || number > 65535) {
		return 0;
	}
	*descriptor = (int) number;
	return 1;
}


static Value primStreamWrite(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	Value count = primitiveArgument(args, 1);
	Value bytes = primitiveArgument(args, 2);
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)
			|| !valueTypeOf(count, VALUE_INT) || !valueTypeOf(bytes, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(bytes);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	intptr_t wanted = asCInt(count);
	if (wanted < 0 || (size_t) wanted > rawObjectElementCount(object)) {
		return PRIMITIVE_FAILED; // out of range fails; the fallback signals
	}
	int64_t written = osFileWrite(descriptor, rawObjectBytes(object),
		(size_t) wanted);
	if (written < 0) {
		return PRIMITIVE_FAILED; // the fallback reads the errno through IoError
	}
	return tagInt((intptr_t) written);
}


static Value primStreamRead(Value *args, uint64_t argc)
{
	if (argc != 4) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	Value count = primitiveArgument(args, 1);
	Value into = primitiveArgument(args, 2);
	Value start = primitiveArgument(args, 3);
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)
			|| !valueTypeOf(count, VALUE_INT) || !valueTypeOf(into, VALUE_POINTER)
			|| !valueTypeOf(start, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(into);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	// One-based, like every index in Smalltalk.
	intptr_t from = asCInt(start) - 1;
	intptr_t wanted = asCInt(count);
	size_t size = rawObjectElementCount(object);
	if (from < 0 || wanted < 0 || (size_t) from + (size_t) wanted > size) {
		return PRIMITIVE_FAILED;
	}
	int64_t got = osFileRead(descriptor, rawObjectBytes(object) + from,
		(size_t) wanted);
	if (got < 0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) got);
}


static Value primStreamClose(Value *args, uint64_t argc)
{
	int descriptor;
	if (argc != 1 || !streamDescriptorOf(primitiveArgument(args, 0), &descriptor)) {
		return PRIMITIVE_FAILED;
	}
	return osFileClose(descriptor) ? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


static Value primStreamFlush(Value *args, uint64_t argc)
{
	int descriptor;
	if (argc != 1 || !streamDescriptorOf(primitiveArgument(args, 0), &descriptor)) {
		return PRIMITIVE_FAILED;
	}
	// A NO-OP that answers the receiver, and deliberately so. Nothing is buffered
	// on the C side; the buffering that exists is the kernel's own, in Smalltalk,
	// and it has already been written out by the time this is sent.
	//
	// Calling through to the OS was the obvious version and it was wrong:
	// fsync on a terminal or a pipe answers EINVAL, so `Transcript flush` failed,
	// the Smalltalk fallback raised an IoError, and reporting THAT needed the
	// Transcript. Measured as an infinite recursion on `last`.
	(void) descriptor;
	return primitiveReceiver(args);
}


// ---------------------------------------------------------------------------
// Characters
// ---------------------------------------------------------------------------
//
// A Character is an IMMEDIATE: the code point IS the payload, so both of these
// are a tag operation and neither touches the heap.

static Value primCharacterCode(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_CHAR)) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) (unsigned char) asCChar(receiver));
}


static Value primCharacterNew(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value code = primitiveArgument(args, 0);
	if (!valueTypeOf(code, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t point = asCInt(code);
	// Out of range FAILS rather than truncating: the Smalltalk fallback is what
	// knows how to signal, and a truncated code point is a wrong character.
	if (point < 0 || point > 255) {
		return PRIMITIVE_FAILED;
	}
	return tagChar((char) point);
}


// ---------------------------------------------------------------------------
// Showing a value, before there is a kernel that can
// ---------------------------------------------------------------------------
//
// The ONE primitive here that packages/ does not name (runtime/Primitives.def
// says so at its line). The VM's built-in kernel has no streams, no printOn:
// and no Transcript, so without this it can run a program and have no way to
// say what came out. It is deliberately NOT wired into packages/Core: that
// kernel's printNl goes through a buffered stream, and a C primitive writing
// straight to the descriptor would interleave with the buffer.
//
// It answers the RECEIVER, like printNl does, so it composes in a cascade.

static Value primPrintValue(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (valueTypeOf(receiver, VALUE_POINTER)) {
		RawObject *object = asObject(receiver);
		if (object == Handles.nil.raw) {
			printf("nil\n");
			return receiver;
		}
		if (object == Handles.true_.raw || object == Handles.false_.raw) {
			printf("%s\n", object == Handles.true_.raw ? "true" : "false");
			return receiver;
		}
		// A String or a Symbol prints its characters. Anything else has no
		// printOn: to reach from C, so it says what it IS rather than guessing:
		// the built-in kernel is minimal on purpose and this is where that shows.
		if (Handles.String.raw != NULL
				&& (rawObjectClassIndex(object) == classIndexOf(&Handles.String)
					|| rawObjectClassIndex(object) == classIndexOf(&Handles.Symbol))) {
			printRawString((RawString *) object);
			printf("\n");
			return receiver;
		}
		printf("a <class %u>\n", rawObjectClassIndex(object));
		return receiver;
	}
	if (valueTypeOf(receiver, VALUE_FLOAT)) {
		// A float PRINTS AS A FLOAT: `7.0`, not `7`. %g drops a trailing `.0`,
		// and a number that answers `7` to printNl and `false` to `= 7` would be
		// a small mystery every time it happened.
		char text[32];
		snprintf(text, sizeof text, "%g", floatValueOf(receiver));
		printf("%s%s\n", text, strpbrk(text, ".eni") == NULL ? ".0" : "");
		return receiver;
	}
	printValue(receiver);
	printf("\n");
	return receiver;
}


// ---------------------------------------------------------------------------
// Indexed access
// ---------------------------------------------------------------------------
//
// One primitive per storage format rather than one polymorphic one, because the
// ANSWER's type differs where the format does not: String and ByteArray are
// both FORMAT_BYTES, and at: answers a Character for one and a SmallInteger for
// the other. The class that installs the primitive is what decides, which is
// where that decision belongs.
//
// Indices are 1-based, and out of range FAILS rather than raising here: the
// Smalltalk fallback is what knows how to signal an IndexOutOfBounds with a
// useful message.

// Named instance variables sit between the element count and the indexed
// elements, so an indexed class that also has named slots has its element zero
// past them. Array has none and pays a load it does not need; a wrong answer
// for the classes that do have them would be far more expensive.
static Value *indexedPointerBase(RawObject *object)
{
	RawClass *class = (RawClass *) classTableAt(&CurrentThread.heap->classes,
		rawObjectClassIndex(object));
	return (Value *) object->body + 1 + class->instanceShape.fixedSlots;
}


// The receiver must be a heap object of `format`, and the index a SmallInteger
// within 1..count. Answers 0 on any failure, and writes the zero-based index.
static _Bool indexedTarget(Value *args, uint64_t argc, uint64_t wanted,
	ObjectFormat format, RawObject **object, size_t *index)
{
	if (argc != wanted) {
		return 0;
	}
	Value receiver = primitiveReceiver(args);
	Value indexValue = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(indexValue, VALUE_INT)) {
		return 0;
	}
	RawObject *target = asObject(receiver);
	if (rawObjectFormat(target) != format) {
		return 0;
	}
	intptr_t oneBased = asCInt(indexValue);
	if (oneBased < 1 || (size_t) oneBased > rawObjectElementCount(target)) {
		return 0;
	}
	*object = target;
	*index = (size_t) oneBased - 1;
	return 1;
}


static Value primBasicSize(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)) {
		return PRIMITIVE_FAILED; // an immediate has no indexed part
	}
	RawObject *object = asObject(receiver);
	switch (rawObjectFormat(object)) {
	case FORMAT_INDEXED_POINTERS:
	case FORMAT_BYTES:
	case FORMAT_DOUBLES:
		return tagInt((intptr_t) rawObjectElementCount(object));
	case FORMAT_NO_POINTERS:
	case FORMAT_POINTERS:
		return tagInt(0); // not indexed, so it has no indexed elements
	default:
		// The MIXED formats put their count somewhere the format alone does not
		// describe (a class's first body word is its superclass, not a count),
		// so reading one here would answer a tagged pointer as a size.
		return PRIMITIVE_FAILED;
	}
}


// ONE polymorphic at:, because that is what the kernel declares: AtPrimitive
// sits on Object and String, ByteArray, Array and FloatArray all inherit it.
//
// The storage format answers almost everything, and the one case it cannot is
// String versus ByteArray: both are FORMAT_BYTES, and at: answers a Character
// for one and a SmallInteger for the other. That is decided by CLASS, which is
// the only place in this file where the format is not enough.
static _Bool receiverIsString(RawObject *object)
{
	uint32_t index = rawObjectClassIndex(object);
	return (Handles.String.raw != NULL && index == classIndexOf(&Handles.String))
		|| (Handles.Symbol.raw != NULL && index == classIndexOf(&Handles.Symbol));
}


static Value primAt(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value indexValue = primitiveArgument(args, 0);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(indexValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(receiver);
	ObjectFormat format = rawObjectFormat(object);
	if (format != FORMAT_INDEXED_POINTERS && format != FORMAT_BYTES
			&& format != FORMAT_DOUBLES) {
		return PRIMITIVE_FAILED; // not indexed at all
	}
	intptr_t oneBased = asCInt(indexValue);
	if (oneBased < 1 || (size_t) oneBased > rawObjectElementCount(object)) {
		return PRIMITIVE_FAILED; // the fallback raises a useful IndexOutOfBounds
	}
	size_t index = (size_t) oneBased - 1;

	switch (format) {
	case FORMAT_INDEXED_POINTERS:
		return indexedPointerBase(object)[index];
	case FORMAT_DOUBLES:
		return floatResult(rawObjectDoubles(object)[index]);
	default:
		return receiverIsString(object)
			? tagChar((char) rawObjectBytes(object)[index])
			: tagInt((intptr_t) rawObjectBytes(object)[index]);
	}
}


static Value primAtPut(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	Value indexValue = primitiveArgument(args, 0);
	Value value = primitiveArgument(args, 1);
	if (!valueTypeOf(receiver, VALUE_POINTER) || !valueTypeOf(indexValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(receiver);
	ObjectFormat format = rawObjectFormat(object);
	intptr_t oneBased = asCInt(indexValue);
	if (format != FORMAT_INDEXED_POINTERS && format != FORMAT_BYTES
			&& format != FORMAT_DOUBLES) {
		return PRIMITIVE_FAILED;
	}
	if (oneBased < 1 || (size_t) oneBased > rawObjectElementCount(object)) {
		return PRIMITIVE_FAILED;
	}
	size_t index = (size_t) oneBased - 1;

	switch (format) {
	case FORMAT_INDEXED_POINTERS:
		// Through the generational write barrier, always. An old Array storing a
		// young pointer without logging it is the bug that survives every test
		// until the young object is collected while still reachable only from
		// old space.
		rawObjectStoreValue(object, &indexedPointerBase(object)[index], value);
		return value;

	case FORMAT_DOUBLES: {
		Number number = numberOf(value);
		if (number.kind == NUM_NOT) {
			return PRIMITIVE_FAILED;
		}
		rawObjectDoubles(object)[index] = number.asFloat;
		return value;
	}

	default:
		if (receiverIsString(object)) {
			if (!valueTypeOf(value, VALUE_CHAR)) {
				return PRIMITIVE_FAILED;
			}
			rawObjectBytes(object)[index] = (uint8_t) (unsigned char) asCChar(value);
			return value;
		}
		if (!valueTypeOf(value, VALUE_INT)) {
			return PRIMITIVE_FAILED;
		}
		intptr_t byte = asCInt(value);
		if (byte < 0 || byte > 255) {
			return PRIMITIVE_FAILED;
		}
		rawObjectBytes(object)[index] = (uint8_t) byte;
		return value;
	}
}


// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------
//
// Designated initialisers, so the array is indexed by the enum and a primitive
// added out of order still lands in its own slot. The old VM's table was
// positional and its comment said in capitals never to reorder it.

static const struct {
	PrimitiveFunction function;
	const char *name;
} gPrimitives[PRIM_COUNT] = {
	[PRIM_NONE] = { NULL, "none" },
#define PRIMITIVE(id, name, function) [PRIM_##id] = { function, name },
#include "runtime/Primitives.def"
#undef PRIMITIVE
};


PrimitiveFunction primitiveFunctionAt(PrimitiveNumber number)
{
	// An out-of-range number is a bug in whoever built the method, and it must
	// not degrade into a send that quietly answers nothing.
	ASSERT(number < PRIM_COUNT);
	// NULL is legal and means DECLARED BUT NOT IMPLEMENTED: the method compiles
	// and runs its Smalltalk fallback. The caller checks; nothing here guesses.
	return gPrimitives[number].function;
}


const char *primitiveName(PrimitiveNumber number)
{
	ASSERT(number < PRIM_COUNT);
	return gPrimitives[number].name;
}


PrimitiveNumber primitiveNumberNamed(const char *name, size_t length)
{
	// Linear over 173 entries, once per method COMPILED, which is not a path
	// worth a hash table until it shows up in a profile.
	for (int i = 1; i < PRIM_COUNT; i++) {
		const char *candidate = gPrimitives[i].name;
		if (strncmp(candidate, name, length) == 0 && candidate[length] == 0) {
			return (PrimitiveNumber) i;
		}
	}
	return PRIM_NONE;
}


void primitiveCoverage(size_t *implemented, size_t *declared)
{
	size_t have = 0;
	for (int i = 1; i < PRIM_COUNT; i++) {
		if (gPrimitives[i].function != NULL) {
			have++;
		}
	}
	if (implemented != NULL) {
		*implemented = have;
	}
	if (declared != NULL) {
		*declared = (size_t) PRIM_COUNT - 1;
	}
}
