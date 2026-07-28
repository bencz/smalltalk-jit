// Gate level 7: primitives, and what happens when one fails.
//
// Until this level `3 + 4` compiled to a SEND with nothing at the far end of
// it. This is the far end.
//
// The shape being proved is the one every Smalltalk has and that this project
// depends on more than most: a method may name a primitive, compiled code
// attempts it right after the prologue, and
//
//   * on SUCCESS it returns the answer and never enters the bytecode;
//   * on FAILURE it falls through into the method's own body, which is the
//     general case written in Smalltalk.
//
// So most of the checks below come in pairs: the primitive answering, and the
// same selector with an operand it cannot handle, answering from the fallback.
// A primitive that quietly approximated instead of failing would pass the first
// half of every pair.
//
// AND THE DISCIPLINE (ADR 0006). Every arithmetic result here arrives through a
// SEND with an inline cache in front of it. Nothing is resolved statically and
// nothing is open-coded at the call site. The last section reads the profile
// back out to prove it: if the cache were being jumped over, as the old VM's
// arithmetic fast path jumped over it, those counts would be zero.

#include "compiler/Bytecode.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "jit/CompiledMethod.h"
#include "jit/InlineCache.h"
#include "jit/Jit.h"
#include "jit/MacroAssembler.h"
#include "memory/Collector.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/Closure.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

__thread Thread CurrentThread;
ptrdiff_t gCurrentThreadTpoff;

static int gFailures;
static int gChecks;

// What a failing primitive's fallback answers, and it is deliberately not a
// value any primitive here could produce.
#define FALLBACK_MARK (-999)


static void check(const char *what, int ok)
{
	gChecks++;
	if (!ok) {
		gFailures++;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  ok    %s\n", what);
	}
}


static void checkInt(const char *what, Value got, intptr_t expected)
{
	int ok = valueTypeOf(got, VALUE_INT) && asCInt(got) == expected;
	check(what, ok);
	if (!ok) {
		printf("        expected %ld, got ", (long) expected);
		printValue(got);
		printf("\n");
	}
}


static void checkFloat(const char *what, Value got, double expected)
{
	int ok = valueTypeOf(got, VALUE_FLOAT) && floatValueOf(got) == expected;
	check(what, ok);
	if (!ok) {
		printf("        expected %g, got ", expected);
		printValue(got);
		printf("\n");
	}
}


// The double behind a float answer, in EITHER representation.
//
// checkFloat above accepts only the immediate one, which is right for every
// everyday value and wrong for the three the divide-by-zero checks are about:
// an infinity and a NaN do not fit the SmallFloat64 window, so they arrive as
// BoxedFloat64 -- which is exactly what tests/SmallFloat64BoundaryTest.st
// asserts about `Float infinity`. A check that demanded the immediate form
// would be asking for a representation the kernel says is impossible.
static _Bool floatOf(Value value, double *answer)
{
	if (valueTypeOf(value, VALUE_FLOAT)) {
		*answer = floatValueOf(value);
		return 1;
	}
	if (valueTypeOf(value, VALUE_POINTER) && Handles.BoxedFloat64.raw != NULL
			&& rawObjectClassIndex(asObject(value))
				== classIndexOf(&Handles.BoxedFloat64)) {
		*answer = ((RawFloat *) asObject(value))->value;
		return 1;
	}
	return 0;
}


static void checkBoolean(const char *what, Value got, _Bool expected)
{
	RawObject *wanted = expected ? Handles.true_.raw : Handles.false_.raw;
	int ok = valueTypeOf(got, VALUE_POINTER) && asObject(got) == wanted;
	check(what, ok);
	if (!ok) {
		printf("        expected %s, got ", expected ? "true" : "false");
		printValue(got);
		printf("\n");
	}
}


// ---- bootstrap --------------------------------------------------------------

static void bootstrapClassOfClasses(Heap *heap)
{
	size_t bytes = objectSizeForShape(CLASS_OF_CLASSES_SHAPE, CLASS_RAW_TRAILER_BYTES);
	RawClass *class = (RawClass *) allocate(heap, bytes);
	uint32_t index = classTableAdd(&heap->classes, (RawObject *) class);
	class->header = makeObjectHeader(index, index, FORMAT_MIXED_BYTES,
		bytes / sizeof(uint64_t));
	class->instanceShape = CLASS_OF_CLASSES_SHAPE;
	class->classIndex = index;
	Handles.ClassClass.raw = class;
}


// A method dictionary, attached in a SECOND pass. It cannot happen at class
// creation: a Dictionary is an ordinary object, so allocating one needs
// Handles.Dictionary and Handles.Array to already name real classes.
static Class *withMethods(Class *class)
{
	Dictionary *methods = newDictionary(8);
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->methodDictionary,
		(RawObject *) methods->raw);
	return class;
}


static Class *defineClass(InstanceShape shape)
{
	return withMethods(classCreate(NULL, NULL, shape));
}


static void bootstrapMinimal(Heap *heap)
{
	bootstrapClassOfClasses(heap);
	InstanceShape bytes = DEFINE_SHAPE(FORMAT_BYTES, 0, 0, 0);
	InstanceShape array = DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, 0);
	InstanceShape fixed0 = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 0);
	InstanceShape fixed2 = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 2);
	// One raw body word holding a double, and NO pointers in it. Getting this
	// format wrong would have the collector chase a mantissa as an address.
	InstanceShape boxedFloat = DEFINE_SHAPE(FORMAT_NO_POINTERS, 0, 0, 1);

	// Object at the root, and everything under it. A real chain and not a flat
	// set, because a primitive installed on Object -- == , class, identityHash --
	// has to be REACHED from a SmallInteger receiver, and reaching it is the
	// superclass walk in jitDispatch doing its job.
	Class *object = classCreate(NULL, NULL, fixed0);
	Handles.ObjectClass.raw = object->raw;
	Handles.String.raw = classCreate(object, NULL, bytes)->raw;
	Handles.Symbol.raw = classCreate(&Handles.String, NULL, bytes)->raw;
	Handles.ByteArray.raw = classCreate(object, NULL, bytes)->raw;
	Handles.Array.raw = classCreate(object, NULL, array)->raw;
	Handles.Association.raw = classCreate(object, NULL, fixed2)->raw;
	Handles.Dictionary.raw = classCreate(object, NULL, fixed2)->raw;
	Handles.CompiledMethod.raw = classCreate(object, NULL, COMPILED_METHOD_SHAPE)->raw;
	Handles.UndefinedObject.raw = classCreate(object, NULL, fixed0)->raw;
	Handles.True.raw = classCreate(object, NULL, fixed0)->raw;
	Handles.False.raw = classCreate(object, NULL, fixed0)->raw;
	Handles.SmallInteger.raw = classCreate(object, NULL, fixed0)->raw;
	Handles.SmallFloat64.raw = classCreate(object, NULL, fixed0)->raw;
	Handles.BoxedFloat64.raw = classCreate(object, NULL, boxedFloat)->raw;
	Handles.Character.raw = classCreate(object, NULL, fixed0)->raw;
	Handles.Closure.raw = classCreate(object, NULL, CLOSURE_SHAPE)->raw;
	Handles.Cell.raw = classCreate(object, NULL, CELL_SHAPE)->raw;

	Handles.symbolTable.raw = newArray(1024)->raw;

	// Now that a Dictionary can be allocated, give one to every class that will
	// hold methods.
	withMethods(&Handles.String);
	withMethods(&Handles.ByteArray);
	withMethods(&Handles.Array);
	withMethods(&Handles.ObjectClass);
	withMethods(&Handles.SmallInteger);
	withMethods(&Handles.SmallFloat64);
	withMethods(&Handles.BoxedFloat64);
	withMethods(&Handles.Character);
	withMethods(&Handles.Closure);

	// IMMORTAL, and that is load-bearing rather than tidy: generated code bakes
	// these three addresses as immediates -- `ifTrue:` is a compare against the
	// true singleton, the prologue fills unused slots with nil -- so they must
	// never move. In the nursery everything works until the first collection,
	// after which a value that IS false stops matching the baked false.
	Handles.nil.raw = ((Object *) newImmortalObject(&Handles.UndefinedObject, 0))->raw;
	Handles.true_.raw = ((Object *) newImmortalObject(&Handles.True, 0))->raw;
	Handles.false_.raw = ((Object *) newImmortalObject(&Handles.False, 0))->raw;

	// Immediates have no header, so their classes are found by tag. Unlike the
	// level-3 test, these are three DISTINCT classes here, because the whole
	// point of the argument-class profile is telling an integer from a float.
	gImmediateClasses.smallInteger = classIndexOf(&Handles.SmallInteger);
	gImmediateClasses.character = classIndexOf(&Handles.Character);
	gImmediateClasses.smallFloat = classIndexOf(&Handles.SmallFloat64);
}


// ---- building methods -------------------------------------------------------

static CodeUnit *makeUnit(Instruction *code, uint16_t count, uint16_t registers,
	uint16_t arguments, Array *literals)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->code = code;
	unit->instructionCount = count;
	unit->registerCount = registers;
	unit->argumentCount = arguments;
	unit->literals = literals != NULL ? tagPtr(literals->raw) : 0;
	return unit;
}


static void defineMethod(Class *class, const char *selector, CodeUnit *unit)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *name = asSymbol(stringFromC(selector));
	CompiledMethod *method = compiledMethodCreate(unit, name, class);
	Dictionary *methods = scopeHandle(asObject(class->raw->methodDictionary));
	symbolDictAtPutObject(methods, name, (Object *) method);
	closeHandleScope(&scope, NULL);
}


typedef enum {
	FALLBACK_CONSTANT, // answers FALLBACK_MARK
	FALLBACK_RECEIVER, // answers self, which proves slot 0 survived the attempt
	FALLBACK_ARGUMENT, // answers the first argument, same for slot 1
} FallbackKind;


// A method that is nothing but a primitive and a fallback.
static void definePrimitive(Class *class, const char *selector,
	PrimitiveNumber primitive, uint16_t argumentCount, FallbackKind fallback)
{
	uint16_t temp = (uint16_t) (argumentCount + 1);
	Instruction *code = calloc(2, sizeof(Instruction));
	switch (fallback) {
	case FALLBACK_RECEIVER:
		code[0] = (Instruction) { OP_MOVE, 0, temp, 0, 0 };
		break;
	case FALLBACK_ARGUMENT:
		code[0] = (Instruction) { OP_MOVE, 0, temp, 1, 0 };
		break;
	default:
		code[0] = (Instruction) { OP_LOADI, 0, temp,
			(uint16_t) (int16_t) FALLBACK_MARK, 0 };
		break;
	}
	code[1] = (Instruction) { OP_RET, 0, temp, 0, 0 };

	CodeUnit *unit = makeUnit(code, 2, (uint16_t) (argumentCount + 2),
		argumentCount, NULL);
	unit->primitive = primitive;
	defineMethod(class, selector, unit);
}


// ---- callers ----------------------------------------------------------------
//
// A send is the ONLY way anything below reaches a primitive. There is no direct
// call into one anywhere in this file, deliberately: the point is not that the
// C function computes the right sum, it is that the whole path -- send,
// dispatch, cache, method entry, primitive attempt -- does.

static Array *selectorLiterals(const char *selector)
{
	Array *literals = newArray(1);
	arrayAtPutObject(literals, 0, (Object *) asSymbol(stringFromC(selector)));
	return literals;
}


// `receiver selector: argument`, with the operands arriving as the caller's own
// arguments so nothing is a compile-time constant.
static NativeCode *binarySender(const char *selector)
{
	Instruction *code = calloc(4, sizeof(Instruction));
	code[0] = (Instruction) { OP_MOVE, 0, 3, 1, 0 };  // r3 := arg0, the receiver
	code[1] = (Instruction) { OP_MOVE, 0, 4, 2, 0 };  // r4 := arg1, the argument
	code[2] = (Instruction) { OP_SEND, 1, 5, 0, 3 };  // r5 := r3 sel: r4
	code[3] = (Instruction) { OP_RET, 0, 5, 0, 0 };
	Opcode unsupported = OP_COUNT;
	NativeCode *sender = jitCompile(makeUnit(code, 4, 6, 2,
		selectorLiterals(selector)), &unsupported);
	ASSERT(sender != NULL);
	return sender;
}


static Value sendBinary(NativeCode *sender, Value receiver, Value argument)
{
	return jitCall2(sender, tagPtr(Handles.nil.raw), receiver, argument);
}


static NativeCode *unarySender(const char *selector)
{
	Instruction *code = calloc(3, sizeof(Instruction));
	code[0] = (Instruction) { OP_MOVE, 0, 2, 1, 0 };
	code[1] = (Instruction) { OP_SEND, 0, 3, 0, 2 };
	code[2] = (Instruction) { OP_RET, 0, 3, 0, 0 };
	Opcode unsupported = OP_COUNT;
	NativeCode *sender = jitCompile(makeUnit(code, 3, 4, 1,
		selectorLiterals(selector)), &unsupported);
	ASSERT(sender != NULL);
	return sender;
}


static Value sendUnary(NativeCode *sender, Value receiver)
{
	return jitCall1(sender, tagPtr(Handles.nil.raw), receiver);
}


// `self selector: index put: value`: the send's receiver is the CALLER's
// receiver, which is how a three-operand send fits in two argument registers.
static NativeCode *keywordSender2(const char *selector)
{
	Instruction *code = calloc(5, sizeof(Instruction));
	code[0] = (Instruction) { OP_MOVE, 0, 3, 0, 0 };
	code[1] = (Instruction) { OP_MOVE, 0, 4, 1, 0 };
	code[2] = (Instruction) { OP_MOVE, 0, 5, 2, 0 };
	code[3] = (Instruction) { OP_SEND, 2, 6, 0, 3 };
	code[4] = (Instruction) { OP_RET, 0, 6, 0, 0 };
	Opcode unsupported = OP_COUNT;
	NativeCode *sender = jitCompile(makeUnit(code, 5, 7, 2,
		selectorLiterals(selector)), &unsupported);
	ASSERT(sender != NULL);
	return sender;
}


int main(void)
{
	Heap heap;
	CurrentThread.tlab.top = NULL;
	CurrentThread.tlab.end = NULL;
	CurrentThread.handleScopes = NULL;
	initRememberedSet(&CurrentThread.rememberedSet);
	initHeap(&heap, &CurrentThread);
	CurrentThread.heap = &heap;
	heap.mutators = &CurrentThread;
	CurrentThread.nextMutator = NULL;
	initHandles();

	printf("gate level 7: primitives, and what happens when one fails\n\n");

	HandleScope outer;
	openHandleScope(&outer);
	bootstrapMinimal(&heap);

	Class *smallInteger = &Handles.SmallInteger;
	Class *smallFloat = &Handles.SmallFloat64;
	Class *boxedFloat = &Handles.BoxedFloat64;

	// The numeric primitives go on BOTH numeric classes, which is what makes
	// `3 + 4.0` and `3.0 + 4` reach the same code from opposite receivers.
	static const struct { const char *selector; PrimitiveNumber primitive; } binary[] = {
		{ "+", PRIM_IntAdd }, { "-", PRIM_IntSub }, { "*", PRIM_IntMul },
		{ "/", PRIM_FloatDiv }, { "//", PRIM_IntFloorDiv }, { "\\\\", PRIM_IntMod },
		// Only < and = : the kernel derives >, <= and >= from < in Smalltalk
		// (Magnitude), so those are not primitives and are not faked here.
		{ "<", PRIM_IntLessThan }, { "=", PRIM_FloatEquals },
		{ "bitAnd:", PRIM_IntAnd }, { "bitOr:", PRIM_IntOr },
		{ "bitXor:", PRIM_IntXor }, { "bitShift:", PRIM_IntShift },
	};
	for (size_t i = 0; i < sizeof(binary) / sizeof(binary[0]); i++) {
		definePrimitive(smallInteger, binary[i].selector, binary[i].primitive, 1,
			FALLBACK_CONSTANT);
		definePrimitive(smallFloat, binary[i].selector, binary[i].primitive, 1,
			FALLBACK_CONSTANT);
		definePrimitive(boxedFloat, binary[i].selector, binary[i].primitive, 1,
			FALLBACK_CONSTANT);
	}

	NativeCode *plus = binarySender("+");
	NativeCode *minus = binarySender("-");
	NativeCode *times = binarySender("*");
	NativeCode *divide = binarySender("/");
	NativeCode *floorDivide = binarySender("//");
	NativeCode *floorModulo = binarySender("\\\\");

	// ---- integer arithmetic, through a send --------------------------------
	printf("  -- integer arithmetic, and every one of these is a SEND\n");
	checkInt("3 + 4", sendBinary(plus, tagInt(3), tagInt(4)), 7);
	checkInt("a negative addend", sendBinary(plus, tagInt(3), tagInt(-10)), -7);
	checkInt("10 - 4", sendBinary(minus, tagInt(10), tagInt(4)), 6);
	checkInt("4 - 10", sendBinary(minus, tagInt(4), tagInt(10)), -6);
	checkInt("6 * 7", sendBinary(times, tagInt(6), tagInt(7)), 42);
	checkInt("-6 * 7", sendBinary(times, tagInt(-6), tagInt(7)), -42);
	checkInt("8 / 4, which divides exactly", sendBinary(divide, tagInt(8), tagInt(4)), 2);

	// ---- where a primitive must FAIL, and the fallback answers -------------
	//
	// Each of these would be a plausible-looking wrong answer if the primitive
	// approximated instead of failing, which is why they are here.
	printf("\n  -- and where it must fail, so the method's own code answers\n");
	intptr_t nearMax = ((intptr_t) 1 << 61) - 1;
	checkInt("a sum past SmallInteger maxVal falls back, it does NOT wrap",
		sendBinary(plus, tagInt(nearMax), tagInt(1)), FALLBACK_MARK);
	checkInt("and so does a product",
		sendBinary(times, tagInt(nearMax), tagInt(2)), FALLBACK_MARK);
	checkInt("a difference past minVal falls back",
		sendBinary(minus, tagInt(-((intptr_t) 1 << 61)), tagInt(1)), FALLBACK_MARK);
	checkInt("maxVal + 0 still succeeds, so the guard is not just refusing big numbers",
		sendBinary(plus, tagInt(nearMax), tagInt(0)), nearMax);
	checkInt("7 / 2 falls back, because the answer is a Fraction and not 3",
		sendBinary(divide, tagInt(7), tagInt(2)), FALLBACK_MARK);
	checkInt("division by zero falls back",
		sendBinary(divide, tagInt(7), tagInt(0)), FALLBACK_MARK);
	checkInt("// by zero falls back",
		sendBinary(floorDivide, tagInt(7), tagInt(0)), FALLBACK_MARK);

	// ---- floor division and modulo, on the operands tests forget -----------
	printf("\n  -- // and \\\\ round toward negative infinity, C division does not\n");
	checkInt("7 // 2", sendBinary(floorDivide, tagInt(7), tagInt(2)), 3);
	checkInt("-7 // 2 is -4, not -3", sendBinary(floorDivide, tagInt(-7), tagInt(2)), -4);
	checkInt("7 // -2 is -4", sendBinary(floorDivide, tagInt(7), tagInt(-2)), -4);
	checkInt("-7 // -2 is 3", sendBinary(floorDivide, tagInt(-7), tagInt(-2)), 3);
	checkInt("6 // 2 is exact and needs no correction",
		sendBinary(floorDivide, tagInt(6), tagInt(2)), 3);
	checkInt("7 \\\\ 2", sendBinary(floorModulo, tagInt(7), tagInt(2)), 1);
	checkInt("-7 \\\\ 2 is 1, not -1", sendBinary(floorModulo, tagInt(-7), tagInt(2)), 1);
	checkInt("7 \\\\ -2 is -1", sendBinary(floorModulo, tagInt(7), tagInt(-2)), -1);
	checkInt("-7 \\\\ -2 is -1", sendBinary(floorModulo, tagInt(-7), tagInt(-2)), -1);

	// ---- floats, and the mixed case ----------------------------------------
	//
	// Mixed integer/float arithmetic is the case the previous VM was measured
	// at a hundred times slower on, because it had no path for it at all and
	// every one went through a generality-coercion send in Smalltalk. Here it
	// is the same primitive, and the operand classification is two tag tests.
	printf("\n  -- floats, including the mixed case\n");
	checkFloat("1.5 + 2.25", sendBinary(plus, tagFloat(1.5), tagFloat(2.25)), 3.75);
	checkFloat("3 + 4.5, an integer receiver and a float argument",
		sendBinary(plus, tagInt(3), tagFloat(4.5)), 7.5);
	checkFloat("4.5 + 3, the other way round",
		sendBinary(plus, tagFloat(4.5), tagInt(3)), 7.5);
	checkFloat("2.5 * 4", sendBinary(times, tagFloat(2.5), tagInt(4)), 10.0);
	checkFloat("7 / 2 as floats answers 3.5, where the integers fell back",
		sendBinary(divide, tagFloat(7.0), tagInt(2)), 3.5);
	checkFloat("1 / 4 with a float receiver", sendBinary(divide, tagFloat(1.0), tagInt(4)), 0.25);
	// A ZERO DIVISOR IS NOT A FAILURE WHEN A FLOAT IS INVOLVED, and this check
	// used to assert the opposite: "float division by zero falls back rather
	// than answering an infinity".
	//
	// THE KERNEL REFUTED IT. packages/Core/src/Magnitudes/Float.st defines the
	// two constants as
	//
	//     class infinity [ ^1.0 / 0.0 ]
	//     class nan      [ ^0.0 / 0.0 ]
	//
	// so a primitive that fails on a zero divisor leaves `Float infinity` with
	// nothing to compute it from -- the Smalltalk fallback raised ZeroDivide, so
	// the ONE expression in the kernel whose whole job is to produce an infinity
	// produced an exception instead. tests/FloatEdgeTest.st then asks for
	// 1.0/0.0, -1.0/0.0 and 0.0/0.0 by name, and five other test files reach the
	// same divide through Json and the cross-representation paths.
	//
	// The rule that stands is the one in runtime/Primitive.h, unchanged: fail
	// rather than GUESS. An infinity is not a guess, it is what IEEE 754
	// division answers, and IEEE 754 division is the operation this primitive
	// implements. What still fails is INTEGER division by zero, checked above,
	// because there the answer really does not exist.
	{
		double quotient = 0.0;
		check("1.0 / 0.0 answers +inf, which is what Float class infinity is",
			floatOf(sendBinary(divide, tagFloat(1.0), tagFloat(0.0)), &quotient)
				&& isinf(quotient) && quotient > 0.0);
		check("-1.0 / 0.0 answers -inf, so the SIGN is the dividend's",
			floatOf(sendBinary(divide, tagFloat(-1.0), tagFloat(0.0)), &quotient)
				&& isinf(quotient) && quotient < 0.0);
		check("0.0 / 0.0 answers a NaN, which is what Float class nan is",
			floatOf(sendBinary(divide, tagFloat(0.0), tagFloat(0.0)), &quotient)
				&& isnan(quotient));
	}

	// A BoxedFloat64: the same arithmetic reaching through a heap object.
	Float *boxed = newObject(boxedFloat, 0);
	boxed->raw->value = 0.5;
	checkFloat("a BoxedFloat64 receiver",
		sendBinary(plus, tagPtr(boxed->raw), tagInt(2)), 2.5);
	checkFloat("a BoxedFloat64 argument",
		sendBinary(plus, tagInt(2), tagPtr(boxed->raw)), 2.5);

	// A result outside the SmallFloat64 window is BOXED, and answered.
	//
	// It used to FAIL here, on the stated grounds that the method's own Smalltalk
	// code would build the box. It could not: BoxedFloat64 declares no methods
	// and no constructor, so nothing in the image can make one, and the fallback
	// had no correct answer available -- `1.0e300 * 10.0` ended in a
	// primitiveFailed. So the boxing moved into the primitive, and these two
	// checks are the pair that keeps the trade honest:
	//
	//   * an ordinary result is still an IMMEDIATE, which is the property that
	//     makes arithmetic allocation-free on the path that matters;
	//   * an extraordinary one is a real BoxedFloat64 carrying the exact value,
	//     rather than a failure the caller cannot recover from.
	//
	// Both operands here are BOXED, because 1e300 is itself outside the window
	// and cannot be written as an immediate at all.
	Float *huge = newObject(boxedFloat, 0);
	huge->raw->value = 1e300;
	Float *tiny = newObject(boxedFloat, 0);
	tiny->raw->value = 1e-300;
	{
		Value product = sendBinary(times, tagPtr(huge->raw), tagPtr(huge->raw));
		// The class test alone settles it: a FAILED primitive runs the fallback,
		// which answers the tagged SmallInteger FALLBACK_MARK, and that is not a
		// pointer at all. Naming the mark here as well would read like a second
		// condition and be one that cannot fail.
		check("a result too large for an immediate is BOXED, not failed",
			valueTypeOf(product, VALUE_POINTER)
				&& rawObjectClassIndex(asObject(product))
					== classIndexOf(boxedFloat));
		check("and the box carries the exact double, which is what makes the "
			"answer usable rather than merely non-failing",
			valueTypeOf(product, VALUE_POINTER)
				&& ((RawFloat *) asObject(product))->value == 1e300 * 1e300);
	}
	checkFloat("while a result that FITS is still an immediate, so the ordinary "
		"path allocates nothing",
		sendBinary(times, tagPtr(huge->raw), tagPtr(tiny->raw)), 1.0);

	// ---- comparison ---------------------------------------------------------
	printf("\n  -- comparison\n");
	NativeCode *less = binarySender("<");
	NativeCode *equal = binarySender("=");
	checkBoolean("3 < 4", sendBinary(less, tagInt(3), tagInt(4)), 1);
	checkBoolean("4 < 3", sendBinary(less, tagInt(4), tagInt(3)), 0);
	checkBoolean("3 < 3", sendBinary(less, tagInt(3), tagInt(3)), 0);
	checkBoolean("-1 < 1, where an unsigned compare would disagree",
		sendBinary(less, tagInt(-1), tagInt(1)), 1);
	checkBoolean("3 = 3", sendBinary(equal, tagInt(3), tagInt(3)), 1);
	checkBoolean("3 = 4", sendBinary(equal, tagInt(3), tagInt(4)), 0);
	checkBoolean("2.5 < 3", sendBinary(less, tagFloat(2.5), tagInt(3)), 1);
	checkBoolean("3 = 3.0 across representations",
		sendBinary(equal, tagInt(3), tagFloat(3.0)), 1);
	checkInt("comparing against a non-number falls back",
		sendBinary(less, tagInt(3), tagPtr(Handles.true_.raw)), FALLBACK_MARK);

	// ---- bit operations -----------------------------------------------------
	printf("\n  -- bit operations\n");
	NativeCode *bitAnd = binarySender("bitAnd:");
	NativeCode *bitOr = binarySender("bitOr:");
	NativeCode *bitXor = binarySender("bitXor:");
	NativeCode *bitShift = binarySender("bitShift:");
	checkInt("12 bitAnd: 10", sendBinary(bitAnd, tagInt(12), tagInt(10)), 8);
	checkInt("12 bitOr: 10", sendBinary(bitOr, tagInt(12), tagInt(10)), 14);
	checkInt("12 bitXor: 10", sendBinary(bitXor, tagInt(12), tagInt(10)), 6);
	checkInt("-1 bitAnd: 255, where the sign bits must survive tagging",
		sendBinary(bitAnd, tagInt(-1), tagInt(255)), 255);
	checkInt("1 bitShift: 10", sendBinary(bitShift, tagInt(1), tagInt(10)), 1024);
	checkInt("1024 bitShift: -10 shifts right",
		sendBinary(bitShift, tagInt(1024), tagInt(-10)), 1);
	checkInt("-1024 bitShift: -10 shifts arithmetically",
		sendBinary(bitShift, tagInt(-1024), tagInt(-10)), -1);
	checkInt("a right shift wider than the payload saturates to -1",
		sendBinary(bitShift, tagInt(-1), tagInt(-200)), -1);
	checkInt("and to 0 for a positive receiver",
		sendBinary(bitShift, tagInt(12345), tagInt(-200)), 0);
	checkInt("a left shift that would leave the range falls back",
		sendBinary(bitShift, tagInt(1), tagInt(61)), FALLBACK_MARK);
	checkInt("1 bitShift: 60 still fits",
		sendBinary(bitShift, tagInt(1), tagInt(60)), (intptr_t) 1 << 60);

	// ---- the frame survives a primitive attempt ----------------------------
	//
	// A failed primitive falls through with the frame exactly as the prologue
	// left it, and a successful one must not disturb its CALLER's frame either.
	// Both are claims about generated code rather than about C, so both are
	// checked by reading slots back after the fact.
	printf("\n  -- the frame, on both sides of the call\n");
	Class *frameProbe = defineClass((InstanceShape) DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 0));
	definePrimitive(frameProbe, "echoReceiver:", PRIM_IntAdd, 1, FALLBACK_RECEIVER);
	definePrimitive(frameProbe, "echoArgument:", PRIM_IntAdd, 1, FALLBACK_ARGUMENT);
	Object *probe = newObject(frameProbe, 0);
	check("a failed primitive leaves the RECEIVER slot intact for the fallback",
		sendBinary(binarySender("echoReceiver:"), tagPtr(probe->raw), tagInt(5))
			== tagPtr(probe->raw));
	checkInt("and the ARGUMENT slot too",
		sendBinary(binarySender("echoArgument:"), tagPtr(probe->raw), tagInt(5)), 5);

	// The caller's own slots, read AFTER the send returns.
	Instruction *afterSend = calloc(4, sizeof(Instruction));
	afterSend[0] = (Instruction) { OP_MOVE, 0, 3, 1, 0 };
	afterSend[1] = (Instruction) { OP_MOVE, 0, 4, 2, 0 };
	afterSend[2] = (Instruction) { OP_SEND, 1, 5, 0, 3 };
	afterSend[3] = (Instruction) { OP_RET, 0, 4, 0, 0 }; // the ARGUMENT, not the result
	Opcode unsupported = OP_COUNT;
	NativeCode *caller = jitCompile(makeUnit(afterSend, 4, 6, 2,
		selectorLiterals("+")), &unsupported);
	checkInt("a successful primitive leaves the CALLER's slots intact",
		jitCall2(caller, tagPtr(Handles.nil.raw), tagInt(3), tagInt(4)), 4);

	// ---- identity and reflection -------------------------------------------
	printf("\n  -- identity and reflection\n");
	// On Object ONLY, so a SmallInteger receiver has to reach them through the
	// superclass walk rather than finding them in its own dictionary.
	definePrimitive(&Handles.ObjectClass, "==", PRIM_Identity, 1, FALLBACK_CONSTANT);
	definePrimitive(&Handles.ObjectClass, "class", PRIM_Class, 0, FALLBACK_CONSTANT);
	definePrimitive(&Handles.ObjectClass, "identityHash", PRIM_Hash, 0,
		FALLBACK_CONSTANT);
	NativeCode *identical = binarySender("==");
	NativeCode *classOfSender = unarySender("class");
	NativeCode *hashSender = unarySender("identityHash");

	checkBoolean("3 == 3, because a SmallInteger IS its value",
		sendBinary(identical, tagInt(3), tagInt(3)), 1);
	checkBoolean("3 == 4", sendBinary(identical, tagInt(3), tagInt(4)), 0);
	Object *one = newObject(&Handles.ObjectClass, 0);
	Object *two = newObject(&Handles.ObjectClass, 0);
	checkBoolean("an object is identical to itself",
		sendBinary(identical, tagPtr(one->raw), tagPtr(one->raw)), 1);
	checkBoolean("and not to another of the same class",
		sendBinary(identical, tagPtr(one->raw), tagPtr(two->raw)), 0);
	check("3 class is SmallInteger, found from the TAG since an immediate has no header",
		sendUnary(classOfSender, tagInt(3)) == tagPtr(smallInteger->raw));
	check("an object's class comes from its header",
		sendUnary(classOfSender, tagPtr(one->raw)) == tagPtr(Handles.ObjectClass.raw));
	checkInt("a SmallInteger hashes as itself", sendUnary(hashSender, tagInt(77)), 77);
	check("an object's identityHash is its header's, and is a SmallInteger",
		valueTypeOf(sendUnary(hashSender, tagPtr(one->raw)), VALUE_INT));

	// ---- indexed access -----------------------------------------------------
	//
	// at:, at:put: and size are the other three ADR 0006 names: sends with an
	// inline cache, never resolved statically. One primitive per storage format,
	// because String and ByteArray share FORMAT_BYTES and at: answers a
	// Character for one and a SmallInteger for the other.
	printf("\n  -- at:, at:put: and size, which are sends like everything else\n");
	definePrimitive(&Handles.Array, "at:", PRIM_At, 1, FALLBACK_CONSTANT);
	definePrimitive(&Handles.Array, "at:put:", PRIM_AtPut, 2, FALLBACK_CONSTANT);
	definePrimitive(&Handles.Array, "basicSize", PRIM_Size, 0, FALLBACK_CONSTANT);
	definePrimitive(&Handles.String, "at:", PRIM_At, 1, FALLBACK_CONSTANT);
	definePrimitive(&Handles.String, "at:put:", PRIM_AtPut, 2, FALLBACK_CONSTANT);
	definePrimitive(&Handles.String, "basicSize", PRIM_Size, 0, FALLBACK_CONSTANT);
	definePrimitive(&Handles.ByteArray, "at:", PRIM_At, 1, FALLBACK_CONSTANT);
	definePrimitive(&Handles.ByteArray, "at:put:", PRIM_AtPut, 2, FALLBACK_CONSTANT);
	definePrimitive(&Handles.ObjectClass, "basicSize", PRIM_Size, 0,
		FALLBACK_CONSTANT);

	NativeCode *at = binarySender("at:");
	NativeCode *atPut = keywordSender2("at:put:");
	NativeCode *basicSize = unarySender("basicSize");

	Array *numbers = newArray(3);
	numbers->raw->vars[0] = tagInt(10);
	numbers->raw->vars[1] = tagInt(20);
	numbers->raw->vars[2] = tagInt(30);
	checkInt("an Array's first element, 1-based",
		sendBinary(at, tagPtr(numbers->raw), tagInt(1)), 10);
	checkInt("and its last", sendBinary(at, tagPtr(numbers->raw), tagInt(3)), 30);
	checkInt("index 0 falls back, because Smalltalk indices start at one",
		sendBinary(at, tagPtr(numbers->raw), tagInt(0)), FALLBACK_MARK);
	checkInt("and so does one past the end",
		sendBinary(at, tagPtr(numbers->raw), tagInt(4)), FALLBACK_MARK);
	checkInt("a non-integer index falls back",
		sendBinary(at, tagPtr(numbers->raw), tagFloat(1.0)), FALLBACK_MARK);
	checkInt("an Array's basicSize", sendUnary(basicSize, tagPtr(numbers->raw)), 3);
	checkInt("a non-indexed object's basicSize is zero",
		sendUnary(basicSize, tagPtr(one->raw)), 0);

	checkInt("at:put: answers the value it stored",
		jitCall2(atPut, tagPtr(numbers->raw), tagInt(2), tagInt(99)), 99);
	checkInt("and the store actually happened",
		sendBinary(at, tagPtr(numbers->raw), tagInt(2)), 99);
	checkInt("at:put: out of range falls back",
		jitCall2(atPut, tagPtr(numbers->raw), tagInt(9), tagInt(99)), FALLBACK_MARK);

	String *word = stringFromC("hi");
	Value first = sendBinary(at, tagPtr(word->raw), tagInt(1));
	check("a String's element is a CHARACTER",
		valueTypeOf(first, VALUE_CHAR) && asCChar(first) == 'h');
	checkInt("a String's basicSize is its byte count",
		sendUnary(basicSize, tagPtr(word->raw)), 2);
	check("at:put: on a String takes a Character",
		jitCall2(keywordSender2("at:put:"), tagPtr(word->raw), tagInt(1), tagChar('H'))
			== tagChar('H'));
	check("and the byte changed", word->raw->contents[0] == 'H');
	checkInt("at:put: on a String rejects a SmallInteger",
		jitCall2(keywordSender2("at:put:"), tagPtr(word->raw), tagInt(1), tagInt(65)),
		FALLBACK_MARK);

	Object *bytes = newObject(&Handles.ByteArray, 4);
	checkInt("a ByteArray's element is a SMALL INTEGER, from the same format",
		jitCall2(keywordSender2("at:put:"), tagPtr(bytes->raw), tagInt(1), tagInt(200)), 200);
	checkInt("and it reads back", sendBinary(at, tagPtr(bytes->raw), tagInt(1)), 200);
	checkInt("a byte past 255 falls back",
		jitCall2(keywordSender2("at:put:"), tagPtr(bytes->raw), tagInt(1), tagInt(256)),
		FALLBACK_MARK);

	// An Array store through the generational write barrier: the receiver is
	// old, the stored value is young, and the remembered set is the only thing
	// that keeps the young object alive when nothing but the old one names it.
	//
	// The two preconditions are CHECKED and not assumed. A version of this that
	// skipped the body when the generations happened not to separate would keep
	// reporting a pass while testing nothing at all, which is the exact way a
	// suite goes blind.
	collectorMarkSweep(&heap); // promotes every survivor, so `numbers` is old
	check("a full collection leaves the surviving Array in the OLD space",
		isOldObject((RawObject *) numbers->raw));
	Object *fresh = newObject(&Handles.ObjectClass, 0);
	check("and an object allocated after it is YOUNG", isNewObject(fresh->raw));
	jitCall2(atPut, tagPtr(numbers->raw), tagInt(1), tagPtr(fresh->raw));
	check("the barrier logged the old-to-young edge",
		rawObjectHasGcBit((RawObject *) numbers->raw, GC_REMEMBERED));
	collectorScavenge(&heap);
	check("so the young object survives a collection that no other root reaches",
		valueTypeOf(numbers->raw->vars[0], VALUE_POINTER)
		&& rawObjectClassIndex(asObject(numbers->raw->vars[0]))
			== classIndexOf(&Handles.ObjectClass));
	check("and the slot was UPDATED to where the object moved",
		numbers->raw->vars[0] == tagPtr(fresh->raw));

	// ---- THE PROFILE, which is why any of this goes through a send ---------
	//
	// ADR 0006, and the reason arithmetic is not an opcode. If `+` were resolved
	// at the call site the way the old VM resolved it, this cell would be empty,
	// because that fast path jumped over the cache whenever it HIT. The profile
	// there was not imprecise, it was the complement of the truth.
	printf("\n  -- the type profile, which is the whole reason these are sends\n");
	NativeCode *profiled = binarySender("+");
	for (int i = 0; i < 20; i++) {
		sendBinary(profiled, tagInt(i), tagInt(1));
	}
	IcCell *cell = &profiled->cells[2];
	check("the arithmetic send site HAS a cache, and it counted every execution",
		cell->sends == 20);
	check("it is monomorphic on SmallInteger", icIsMonomorphic(cell)
		&& cell->ways[0].classIndex == classIndexOf(smallInteger));
	check("and it knows the ARGUMENT is a SmallInteger too, which is what "
		"chooses integer over floating-point arithmetic",
		icDominantArgumentClass(cell) == classIndexOf(smallInteger));

	NativeCode *mixed = binarySender("+");
	for (int i = 0; i < 10; i++) {
		sendBinary(mixed, tagInt(i), tagFloat(0.5));
	}
	IcCell *mixedCell = &mixed->cells[2];
	check("a site whose argument is always a Float says so, from the same receiver class",
		mixedCell->ways[0].classIndex == classIndexOf(smallInteger)
		&& icDominantArgumentClass(mixedCell) == classIndexOf(smallFloat));

	NativeCode *poly = binarySender("+");
	for (int i = 0; i < 6; i++) {
		sendBinary(poly, tagInt(i), tagInt(1));
	}
	sendBinary(poly, tagFloat(1.0), tagInt(1));
	sendBinary(poly, tagPtr(boxed->raw), tagInt(1));
	IcCell *polyCell = &poly->cells[2];
	check("a site with three receiver classes records three ways",
		polyCell->wayCount == 3 && !icIsMonomorphic(polyCell));
	double fraction = 0.0;
	check("with the dominant one identified by COUNT and not by arrival order",
		icDominantClass(polyCell, &fraction) == classIndexOf(smallInteger)
		&& fraction > 0.74 && fraction < 0.76);

	// ---- the NAMES, which are the contract with packages/ -------------------
	//
	// A primitive is reached from Smalltalk by pragma, `<primitive: IntAddPrimitive>`,
	// and runtime/Primitives.def was extracted from packages/ rather than
	// invented. These checks are what keeps the two from drifting.
	printf("\n  -- the names packages/ actually writes\n");
	check("a kernel name resolves to its primitive",
		primitiveNumberNamed("IntAddPrimitive", 15) == PRIM_IntAdd
		&& primitiveNumberNamed("BlockValuePrimitive", 19) == PRIM_BlockValue);
	check("a name that is only a PREFIX of a real one does not match",
		primitiveNumberNamed("IntAdd", 6) == PRIM_NONE);
	check("a name nobody declares answers PRIM_NONE rather than guessing",
		primitiveNumberNamed("NoSuchPrimitive", 15) == PRIM_NONE);
	check("Int and Float arithmetic share ONE implementation, which is what makes "
		"mixed arithmetic a fast path instead of a coercion send",
		primitiveFunctionAt(PRIM_IntAdd) == primitiveFunctionAt(PRIM_FloatAdd)
		&& primitiveFunctionAt(PRIM_IntLessThan) == primitiveFunctionAt(PRIM_FloatLessThan));
	{
		// SEARCHED, not named. This used to point at PRIM_FloatSin, and the check
		// then failed the day sin was implemented: it was asserting a fact about
		// one primitive when what it means to assert is a fact about the TABLE,
		// that a declared name with no function is still a real number so the
		// method compiles and runs its fallback.
		PrimitiveNumber unimplemented = PRIM_NONE;
		for (int i = 1; i < PRIM_COUNT; i++) {
			if (primitiveFunctionAt((PrimitiveNumber) i) == NULL) {
				unimplemented = (PrimitiveNumber) i;
				break;
			}
		}
		check("a DECLARED but unimplemented primitive is a real number with no "
			"function, so the method compiles and runs its fallback",
			unimplemented != PRIM_NONE
			&& primitiveFunctionAt(unimplemented) == NULL);

		size_t implemented = 0, declared = 0;
		primitiveCoverage(&implemented, &declared);
		printf("        primitives: %zu of %zu declared by packages/ are implemented\n",
			implemented, declared);
		check("every name packages/ uses is declared here, plus the one the built-in "
				"kernel needs and packages/ does not", declared == 175);
	}

	// ---- closures, at the bytecode level ------------------------------------
	//
	// ADR 0008: a block captures BY VALUE into itself, and a variable that is
	// captured AND MUTATED gets a cell. Proved here on hand-written bytecode,
	// before the front end emits any, for the same reason every level below did
	// it that way: the mechanism can be wrong in ways the source level would
	// hide.
	printf("\n  -- closures: flat captures, and cells for what is mutated\n");
	definePrimitive(&Handles.Closure, "value", PRIM_BlockValue, 0, FALLBACK_CONSTANT);
	definePrimitive(&Handles.Closure, "value:", PRIM_BlockValue1, 1, FALLBACK_CONSTANT);

	// The block: register 0 is the CLOSURE, so GETUP reaches a capture with one
	// load and no chain to walk.
	//   ^captured0 + captured1
	static Instruction blockBody[] = {
		{ OP_GETUP, 0, 1, 0, 0 },   // r1 := captured[0]
		{ OP_GETUP, 0, 3, 1, 0 },   // r3 := captured[1]  (the send's argument)
		{ OP_MOVE, 0, 2, 1, 0 },    // r2 := r1           (the send's receiver)
		{ OP_SEND, 1, 4, 0, 2 },    // r4 := r2 + r3
		{ OP_RET, 0, 4, 0, 0 },
	};
	CodeUnit *blockUnit = makeUnit(blockBody, 5, 5, 0, selectorLiterals("+"));
	CompiledMethod *blockMethod = compiledMethodCreate(blockUnit,
		asSymbol(stringFromC("aBlock")), &Handles.Closure);

	// The enclosing method holds that block in its `blocks` array and closes over
	// two of its own registers.
	Array *blocks = newArray(1);
	arrayAtPutObject(blocks, 0, (Object *) blockMethod);
	static Instruction makesClosure[] = {
		{ OP_MOVE, 0, 3, 1, 0 },      // r3 := arg0   (capture 0)
		{ OP_MOVE, 0, 4, 2, 0 },      // r4 := arg1   (capture 1)
		{ OP_CLOSURE, 2, 5, 0, 3 },   // r5 := closure over blocks[0], regs 3..4
		{ OP_MOVE, 0, 6, 5, 0 },
		{ OP_SEND, 0, 7, 0, 6 },      // r7 := r6 value
		{ OP_RET, 0, 7, 0, 0 },
	};
	CodeUnit *closureUnit = makeUnit(makesClosure, 6, 8, 2, selectorLiterals("value"));
	closureUnit->blocks = tagPtr(blocks->raw);
	NativeCode *makesClosureCode = jitCompile(closureUnit, &unsupported);
	check("a method that builds a closure compiles", makesClosureCode != NULL);
	checkInt("the block sees BOTH captured values, by value and with no chain",
		jitCall2(makesClosureCode, tagPtr(Handles.nil.raw), tagInt(30), tagInt(12)), 42);

	// A CELL: the outer method mutates the variable after capturing it, so the
	// closure has to see the NEW value. Capturing by value would answer 1.
	static Instruction cellBlock[] = {
		{ OP_GETUP, 0, 1, 0, 0 },   // r1 := the captured CELL
		{ OP_GETCELL, 0, 2, 1, 0 }, // r2 := its contents
		{ OP_RET, 0, 2, 0, 0 },
	};
	CodeUnit *cellBlockUnit = makeUnit(cellBlock, 3, 3, 0, NULL);
	CompiledMethod *cellBlockMethod = compiledMethodCreate(cellBlockUnit,
		asSymbol(stringFromC("cellBlock")), &Handles.Closure);
	Array *cellBlocks = newArray(1);
	arrayAtPutObject(cellBlocks, 0, (Object *) cellBlockMethod);

	static Instruction mutatesAfterCapture[] = {
		{ OP_LOADI, 0, 1, 1, 0 },      // r1 := 1
		{ OP_NEWCELL, 0, 1, 1, 0 },    // r1 := cell holding 1
		{ OP_MOVE, 0, 2, 1, 0 },       // the capture run
		{ OP_CLOSURE, 1, 3, 0, 2 },    // r3 := closure capturing the CELL
		{ OP_LOADI, 0, 4, 99, 0 },
		{ OP_SETCELL, 0, 1, 4, 0 },    // cell := 99, AFTER the closure exists
		{ OP_MOVE, 0, 5, 3, 0 },
		{ OP_SEND, 0, 6, 0, 5 },       // r6 := r5 value
		{ OP_RET, 0, 6, 0, 0 },
	};
	CodeUnit *cellUnit = makeUnit(mutatesAfterCapture, 9, 7, 0, selectorLiterals("value"));
	cellUnit->blocks = tagPtr(cellBlocks->raw);
	NativeCode *cellCode = jitCompile(cellUnit, &unsupported);
	checkInt("a mutated capture goes through a CELL, so the block sees 99 and not 1",
		jitCall0(cellCode, tagPtr(Handles.nil.raw)), 99);

	// A block with an argument, so the block's own registers and its captures
	// coexist: register 0 is the closure, 1 is the argument.
	static Instruction argBlock[] = {
		{ OP_GETUP, 0, 2, 0, 0 },   // r2 := captured[0]
		{ OP_MOVE, 0, 3, 2, 0 },
		{ OP_MOVE, 0, 4, 1, 0 },    // the block's own argument
		{ OP_SEND, 1, 5, 0, 3 },    // r5 := captured + argument
		{ OP_RET, 0, 5, 0, 0 },
	};
	CodeUnit *argBlockUnit = makeUnit(argBlock, 5, 6, 1, selectorLiterals("+"));
	CompiledMethod *argBlockMethod = compiledMethodCreate(argBlockUnit,
		asSymbol(stringFromC("argBlock")), &Handles.Closure);
	Array *argBlocks = newArray(1);
	arrayAtPutObject(argBlocks, 0, (Object *) argBlockMethod);
	static Instruction callsWithArg[] = {
		{ OP_MOVE, 0, 2, 1, 0 },
		{ OP_CLOSURE, 1, 3, 0, 2 },
		{ OP_MOVE, 0, 4, 3, 0 },
		{ OP_LOADI, 0, 5, 5, 0 },
		{ OP_SEND, 1, 6, 0, 4 },    // r6 := r4 value: 5
		{ OP_RET, 0, 6, 0, 0 },
	};
	CodeUnit *argUnit = makeUnit(callsWithArg, 6, 7, 1, selectorLiterals("value:"));
	argUnit->blocks = tagPtr(argBlocks->raw);
	NativeCode *argCode = jitCompile(argUnit, &unsupported);
	checkInt("value: passes the block's argument alongside its captures",
		jitCall1(argCode, tagPtr(Handles.nil.raw), tagInt(37)), 42);
	checkInt("value with the WRONG arity falls back rather than reading a slot "
		"the block does not have",
		jitCall2(makesClosureCode, tagPtr(Handles.nil.raw), tagInt(1), tagInt(2)), 3);

	// A closure is an ordinary object, so it survives collection like one, and
	// its captures are ordinary tagged slots the collector walks.
	collectorMarkSweep(&heap);
	checkInt("a closure and its captures survive a full collection",
		jitCall2(makesClosureCode, tagPtr(Handles.nil.raw), tagInt(40), tagInt(2)), 42);

	// ---- the primitive sequence on a FOREIGN ABI ---------------------------
	//
	// What a vtable buys and #ifdef does not (ADR 0009): the Win64 sequence is
	// emitted and inspected on a host that cannot run it. It is worth doing for
	// THIS sequence in particular, because the primitive call stages an address
	// in a register after setting up arguments, and the scratch register here is
	// RCX -- SysV's fourth argument register, and Win64's FIRST. Written the
	// obvious way, it assembled, ran correctly on this host, and would have
	// passed the wrong receiver on Windows.
	printf("\n  -- the same primitive call emitted for a foreign ABI\n");
	Instruction *crossCode = calloc(2, sizeof(Instruction));
	crossCode[0] = (Instruction) { OP_LOADI, 0, 2, 0, 0 };
	crossCode[1] = (Instruction) { OP_RET, 0, 2, 0, 0 };
	CodeUnit *crossUnit = makeUnit(crossCode, 2, 3, 1, NULL);
	crossUnit->primitive = PRIM_IntAdd;
	const MacroAssemblerOps *sysv = maBackendNamed("x64");
	const MacroAssemblerOps *win64 = maBackendNamed("x64-win64");
	NativeCode *forSysV = jitCompileFor(sysv, crossUnit, &unsupported);
	NativeCode *forWin64 = jitCompileFor(win64, crossUnit, &unsupported);
	check("a primitive-bearing method compiles for both ABIs",
		forSysV != NULL && forWin64 != NULL && forWin64->size > 0);
	check("and the two sequences DIFFER, because the argument registers do",
		forSysV->size != forWin64->size
		|| memcmp(forSysV->entry, forWin64->entry, forSysV->size) != 0);

	uint64_t before = polyCell->sends;
	collectorScavenge(&heap);
	collectorScavenge(&heap);
	check("and the profile SURVIVES collection, because a way holds a class index",
		polyCell->sends == before && polyCell->wayCount == 3);

	closeHandleScope(&outer, NULL);
	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
