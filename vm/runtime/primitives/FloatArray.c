// FloatArray: a packed array of raw IEEE doubles.
//
// THE SHAPE IS THE POINT. packages/Core/src/Collections/FloatArray.st declares
// `<shape: BytesShape>`, so the payload is 8 bytes per element with no headers,
// no tags and NOTHING FOR THE COLLECTOR TO SCAN. An Array of Floats costs a
// pointer per element plus a boxed object each; this costs eight bytes and the
// collector walks past it.
//
// The consequence the kernel states out loud: `basicSize` answers BYTES,
// because that is what the shape stores, and `size` answers ELEMENTS. Every
// bound check here is in ELEMENTS, and the two must not be confused -- a check
// against basicSize would let an index eight times too large through.
//
// WHY THE DESTINATION IS ALLOCATED IN SMALLTALK. The bulk operations take the
// result array as an argument rather than making one, which keeps every kernel
// below a pure loop over raw buffers: no allocation, so no collection, so no
// way for a buffer pointer taken at the top of the loop to go stale halfway
// down it. `primBinary:into:op:` reads oddly at the Smalltalk level and that is
// the reason for it.
//
// FAILURE IS THE KERNEL'S JOB. Every primitive here fails rather than guessing,
// and the .st fallback decides which error it was: `at:` cannot tell a bad index
// from a bad value, so `indexError:` does it, and `at:put:` converts a Fraction
// through `asFloat` -- a SEND, which a primitive must not make.

#include "runtime/primitives/Shared.h"
#include <math.h>

// The elements of a FloatArray, or 0 for anything that is not one.
//
// BY SHAPE and not by class, exactly as the polymorphic `at:` decides: what
// matters is the layout about to be indexed. A byte object whose length is not
// a multiple of eight cannot be a FloatArray, which is what keeps a String out.
static _Bool floatArrayElements(Value value, double **elements, size_t *count)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return 0;
	}
	size_t bytes = rawObjectElementCount(object);
	if (bytes % sizeof(double) != 0) {
		return 0;
	}
	*elements = (double *) rawObjectBytes(object);
	*count = bytes / sizeof(double);
	return 1;
}


// Any Number the tower represents exactly as a double operand: a Float, either
// immediate or boxed, or a SmallInteger. Anything else fails into the Smalltalk
// fallback, which converts it or raises with the precise message.
static _Bool floatArrayValue(Value value, double *out)
{
	Number number = numberOf(value);
	if (number.kind == NUM_NOT) {
		return 0;
	}
	*out = number.asFloat;
	return 1;
}


Value primFloatArrayAt(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	double *elements;
	size_t count;
	Value index = primitiveArgument(args, 0);
	if (!floatArrayElements(primitiveReceiver(args), &elements, &count)
			|| !valueTypeOf(index, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t i = asCInt(index);
	if (i < 1 || (size_t) i > count) {
		return PRIMITIVE_FAILED; // the kernel's indexError: raises
	}
	FLOAT_RESULT(args, elements[i - 1]);
}


Value primFloatArrayAtPut(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	double *elements;
	size_t count;
	double value;
	Value index = primitiveArgument(args, 0);
	if (!floatArrayElements(primitiveReceiver(args), &elements, &count)
			|| !valueTypeOf(index, VALUE_INT)
			|| !floatArrayValue(primitiveArgument(args, 1), &value)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t i = asCInt(index);
	if (i < 1 || (size_t) i > count) {
		return PRIMITIVE_FAILED;
	}
	elements[i - 1] = value;
	// NO WRITE BARRIER: the payload holds raw doubles and never an object
	// pointer, which is the whole point of the shape. A barrier here would be
	// remembering a store that can never name a young object.
	return primitiveArgument(args, 1);
}


// Reductions. This is where the win is: no send per element, no tag decode, no
// boxing of intermediates, and nothing for the collector to scan.
enum {
	FLOAT_ARRAY_SUM = 1,
	FLOAT_ARRAY_MAX = 2,
	FLOAT_ARRAY_MIN = 3,
};


Value primFloatArrayReduce(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	double *elements;
	size_t count;
	Value opcode = primitiveArgument(args, 0);
	if (!floatArrayElements(primitiveReceiver(args), &elements, &count)
			|| !valueTypeOf(opcode, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t op = asCInt(opcode);
	if (op == FLOAT_ARRAY_SUM) {
		double sum = 0.0;
		for (size_t i = 0; i < count; i++) {
			sum += elements[i];
		}
		FLOAT_RESULT(args, sum);
	}
	if (op != FLOAT_ARRAY_MAX && op != FLOAT_ARRAY_MIN) {
		return PRIMITIVE_FAILED;
	}
	// Max and min have NO IDENTITY ELEMENT, so an empty array fails into the
	// Smalltalk fallback, which raises EmptyCollectionError. Answering zero
	// would be a wrong answer rather than a missing one.
	if (count == 0) {
		return PRIMITIVE_FAILED;
	}
	double best = elements[0];
	for (size_t i = 1; i < count; i++) {
		if (op == FLOAT_ARRAY_MAX ? elements[i] > best : elements[i] < best) {
			best = elements[i];
		}
	}
	FLOAT_RESULT(args, best);
}


Value primFloatArrayDot(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	double *a, *b;
	size_t countA, countB;
	if (!floatArrayElements(primitiveReceiver(args), &a, &countA)
			|| !floatArrayElements(primitiveArgument(args, 0), &b, &countB)
			|| countA != countB) {
		return PRIMITIVE_FAILED;
	}
	double sum = 0.0;
	for (size_t i = 0; i < countA; i++) {
		sum += a[i] * b[i];
	}
	FLOAT_RESULT(args, sum);
}


// Elementwise and scalar arithmetic. ONE primitive with an opcode rather than
// four, because the .def is append-only forever and a family of near-identical
// kernels is exactly the case for spending one entry.
enum {
	FLOAT_ARRAY_ADD = 1,
	FLOAT_ARRAY_SUB = 2,
	FLOAT_ARRAY_MUL = 3,
	FLOAT_ARRAY_DIV = 4,
};


static double floatArrayApply(intptr_t op, double x, double y)
{
	switch (op) {
	case FLOAT_ARRAY_ADD: return x + y;
	case FLOAT_ARRAY_SUB: return x - y;
	case FLOAT_ARRAY_MUL: return x * y;
	default:              return x / y; // DIV, and IEEE: a zero divisor is inf
	}
}


// FloatArray>>primBinary: other into: result op: opcode
Value primFloatArrayBinary(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	double *a, *b, *destination;
	size_t countA, countB, countDestination;
	Value opcode = primitiveArgument(args, 2);
	if (!floatArrayElements(primitiveReceiver(args), &a, &countA)
			|| !floatArrayElements(primitiveArgument(args, 0), &b, &countB)
			|| !floatArrayElements(primitiveArgument(args, 1), &destination,
				&countDestination)
			|| !valueTypeOf(opcode, VALUE_INT)
			|| countA != countB || countA != countDestination) {
		return PRIMITIVE_FAILED;
	}
	intptr_t op = asCInt(opcode);
	if (op < FLOAT_ARRAY_ADD || op > FLOAT_ARRAY_DIV) {
		return PRIMITIVE_FAILED;
	}
	for (size_t i = 0; i < countA; i++) {
		destination[i] = floatArrayApply(op, a[i], b[i]);
	}
	return primitiveArgument(args, 1);
}


// FloatArray>>primScalar: scalar into: result op: opcode
Value primFloatArrayScalar(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	double *a, *destination;
	size_t countA, countDestination;
	double scalar;
	Value opcode = primitiveArgument(args, 2);
	if (!floatArrayElements(primitiveReceiver(args), &a, &countA)
			|| !floatArrayElements(primitiveArgument(args, 1), &destination,
				&countDestination)
			|| !floatArrayValue(primitiveArgument(args, 0), &scalar)
			|| !valueTypeOf(opcode, VALUE_INT)
			|| countA != countDestination) {
		return PRIMITIVE_FAILED;
	}
	intptr_t op = asCInt(opcode);
	if (op < FLOAT_ARRAY_ADD || op > FLOAT_ARRAY_DIV) {
		return PRIMITIVE_FAILED;
	}
	for (size_t i = 0; i < countA; i++) {
		destination[i] = floatArrayApply(op, a[i], scalar);
	}
	return primitiveArgument(args, 1);
}


// FloatArray>>atAllPut: aNumber
Value primFloatArrayFill(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	double *elements;
	size_t count;
	double value;
	if (!floatArrayElements(primitiveReceiver(args), &elements, &count)
			|| !floatArrayValue(primitiveArgument(args, 0), &value)) {
		return PRIMITIVE_FAILED;
	}
	for (size_t i = 0; i < count; i++) {
		elements[i] = value;
	}
	return primitiveReceiver(args);
}
