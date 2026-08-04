// Running a CompiledMethod that was found rather than sent.
//
// `anObject perform: #foo` and `Compiler>>evaluate:` both end here: something
// has a CompiledMethod in hand and a receiver to run it against, with no send
// site and therefore no inline cache. That is the whole difference from an
// ordinary send, and it is why these are primitives rather than bytecode.
//
// THE METHOD IS NOT LOOKED UP HERE. The caller already did that
// (`self class lookupSelector: aSymbol`), so this neither searches a class
// chain nor checks that the method belongs to the receiver's class. That is
// deliberate: `perform:` is how a program calls something it chose, and
// second-guessing the choice would make the reflective path differ from the
// direct one.

#include "runtime/primitives/Shared.h"
#include "jit/CompiledMethod.h"
#include "runtime/Collection.h"
#include "runtime/Closure.h"

// Receiver must be a CompiledMethod with runnable code. Answers NULL when it is
// not one, or when the tier cannot compile it.
static RawCompiledMethod *runnableMethod(Value receiver, uint64_t wantedArguments)
{
	if (!valueTypeOf(receiver, VALUE_POINTER) || Handles.CompiledMethod.raw == NULL
			|| rawObjectClassIndex(asObject(receiver))
				!= classIndexOf(&Handles.CompiledMethod)) {
		return NULL;
	}
	RawCompiledMethod *method = (RawCompiledMethod *) asObject(receiver);
	if (method->unit == NULL || method->unit->argumentCount != wantedArguments) {
		return NULL; // wrong arity: the kernel's fallback raises
	}
	if (method->native == NULL) {
		Opcode unsupported;
		method->native = jitCompile(method->unit, &unsupported);
		if (method->native == NULL) {
			return NULL;
		}
	}
	return method;
}


// CompiledMethod>>sendTo: anObject
Value primMethodSend(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	// The body runs arbitrary Smalltalk, so it allocates as freely as any method
	// and the caller's compiled frames have to be anchored first.
	PRIMITIVE_ALLOCATES(args);
	RawCompiledMethod *method = runnableMethod(primitiveReceiver(args), 0);
	Value answer = method == NULL
		? PRIMITIVE_FAILED
		// The receiver is re-read: compiling does not allocate on the heap today,
		// but nothing here should depend on that staying true.
		: jitCall0(method->native, primitiveArgument(args, 0));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// A wide method called reflectively: the arguments have to be laid out the way
// a compiled caller's frame already has them, receiver highest and the rest at
// descending addresses (jit/Jit.h). An Array holds them the other way round, so
// this is a reversal into a block and not a pointer into the Array.
//
// MALLOC'D AND NOT A STACK BUFFER, because the arity of a wide method has no
// ceiling and a fixed buffer would put one back in exactly the reflective path
// whose whole job is not to be narrower than the direct one.
//
// The block is filled and used with NOTHING allocating in between. It is C
// memory the collector does not know about, so a collection between the copy
// and the call would leave it holding addresses of objects that had moved --
// the same rule the narrow path below states, and the reason both read out of
// the Array as late as possible.
static Value sendWide(RawCompiledMethod *method, Value receiver, RawArray *array,
	size_t count)
{
	Value *block = malloc((count + 1) * sizeof(Value));
	if (block == NULL) {
		return PRIMITIVE_FAILED;
	}
	block[count] = receiver;
	for (size_t i = 0; i < count; i++) {
		block[count - 1 - i] = array->vars[i];
	}
	Value answer = jitCallWide(method->native, &block[count]);
	free(block);
	return answer;
}


// CompiledMethod>>sendTo: anObject withArguments: anArray
//
// NO ARITY CEILING, and that is the point: a send of any arity works here for
// the same reason it works from bytecode, because both go through whichever of
// the two calling conventions the method was compiled with. This used to stop
// at receiver plus five and FAIL above it, which made the reflective path
// narrower than the direct one.
Value primMethodSendArgs(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	Value arguments = primitiveArgument(args, 1);
	if (!valueTypeOf(arguments, VALUE_POINTER)
			|| rawObjectFormat(asObject(arguments)) != FORMAT_INDEXED_POINTERS) {
		return PRIMITIVE_FAILED;
	}
	size_t count = rawObjectElementCount(asObject(arguments));

	PRIMITIVE_ALLOCATES(args);
	RawCompiledMethod *method = runnableMethod(primitiveReceiver(args), count);
	if (method == NULL) {
		PRIMITIVE_DONE_ALLOCATING();
		return PRIMITIVE_FAILED;
	}
	// COPIED OUT BEFORE THE CALL, and out of the Array each time rather than
	// through a saved pointer: entering the method allocates, so an Array read
	// afterwards would be read through an address a collection may have moved.
	Value receiver = primitiveArgument(args, 0);
	RawArray *array = (RawArray *) asObject(primitiveArgument(args, 1));

	Value answer;
	if (method->native->wide) {
		answer = sendWide(method, receiver, array, count);
	} else {
		Value slot[JIT_MAX_NARROW_ARGS];
		for (size_t i = 0; i < count; i++) {
			slot[i] = array->vars[i];
		}
		switch (count) {
		case 0: answer = jitCall0(method->native, receiver); break;
		case 1: answer = jitCall1(method->native, receiver, slot[0]); break;
		case 2: answer = jitCall2(method->native, receiver, slot[0], slot[1]); break;
		case 3: answer = jitCall3(method->native, receiver, slot[0], slot[1], slot[2]);
			break;
		case 4: answer = jitCall4(method->native, receiver, slot[0], slot[1], slot[2],
			slot[3]); break;
		default: answer = jitCall5(method->native, receiver, slot[0], slot[1], slot[2],
			slot[3], slot[4]); break;
		}
	}
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// Reflection over compiled code
// ---------------------------------------------------------------------------
//
// WHAT THE OLD `header` WORD PACKED lives in the CodeUnit now (argument count,
// primitive number, the flags), and a CodeUnit is a C struct rather than a heap
// object, so every one of these is a primitive where v1 had a field read. The
// alternative was what CompiledCode.st said until now: `notYetImplemented` on
// nine accessors, which is the whole reflective protocol answering nothing.
//
// They accept a CompiledMethod OR a CompiledBlock, because the two are the same
// object with different classes and every question here is about the unit.

static CodeUnit *unitOf(Value receiver)
{
	if (!valueTypeOf(receiver, VALUE_POINTER)) {
		return NULL;
	}
	uint32_t index = rawObjectClassIndex(asObject(receiver));
	_Bool isCode = (Handles.CompiledMethod.raw != NULL
			&& index == classIndexOf(&Handles.CompiledMethod))
		|| (Handles.CompiledBlock.raw != NULL
			&& index == classIndexOf(&Handles.CompiledBlock));
	if (!isCode) {
		return NULL;
	}
	return ((RawCompiledMethod *) asObject(receiver))->unit;
}


// One shape for the six that answer a fact about the unit, because the receiver
// check and the "no unit" case are the same in all of them.
#define UNIT_PRIMITIVE(name, expression)                          \
	Value name(Value *args, uint64_t argc)                        \
	{                                                             \
		if (argc != 0) {                                          \
			return PRIMITIVE_FAILED;                              \
		}                                                         \
		CodeUnit *unit = unitOf(primitiveReceiver(args));         \
		if (unit == NULL) {                                       \
			return PRIMITIVE_FAILED;                              \
		}                                                         \
		return (expression);                                      \
	}


// CompiledCode>>argumentsSize
UNIT_PRIMITIVE(primCodeArgumentCount, tagInt((intptr_t) unit->argumentCount))

// CompiledCode>>primitive -- the NUMBER, or nil for a method that has none.
UNIT_PRIMITIVE(primCodePrimitiveNumber, unit->primitive == PRIM_NONE
	? tagPtr(Handles.nil.raw) : tagInt((intptr_t) unit->primitive))

// CompiledCode>>hasContext
//
// `couldBeHome` is the same question asked from the other side: it is set when
// a method contains a block with a non-local return, which is exactly when an
// activation has to be findable by one.
UNIT_PRIMITIVE(primCodeHasContext, tagPtr(unit->couldBeHome
	? Handles.true_.raw : Handles.false_.raw))

// CompiledCode>>literals
UNIT_PRIMITIVE(primCodeLiterals, valueTypeOf(unit->literals, VALUE_POINTER)
	? unit->literals : tagPtr(Handles.nil.raw))

// CompiledCode>>sourceCode
UNIT_PRIMITIVE(primCodeSource, valueTypeOf(unit->source, VALUE_POINTER)
	? unit->source : tagPtr(Handles.nil.raw))

// CompiledBlock>>method -- the method the block was WRITTEN in, nil for a block
// compiled outside any method (a top-level one in a script).
UNIT_PRIMITIVE(primCodeHomeMethod, valueTypeOf(unit->homeMethod, VALUE_POINTER)
	? unit->homeMethod : tagPtr(Handles.nil.raw))


// CompiledCode>>outerReturns
//
// Whether this code itself does a NON-LOCAL RETURN, which is a property of the
// instructions and not of a flag: OP_RETOUTER is emitted for `^` inside a
// block, and a method's own `^` is an ordinary OP_RET.
Value primCodeOuterReturns(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	CodeUnit *unit = unitOf(primitiveReceiver(args));
	if (unit == NULL) {
		return PRIMITIVE_FAILED;
	}
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		if ((Opcode) unit->code[i].op == OP_RETOUTER) {
			return tagPtr(Handles.true_.raw);
		}
	}
	return tagPtr(Handles.false_.raw);
}


// CompiledCode>>descriptors
//
// The bci-to-source map, as an Array of positions parallel to the bytecode.
// EMPTY TODAY AND SAYS SO: CodeUnit.sourcePositions is declared and nothing
// fills it yet, so the honest answer is an Array with no entries rather than an
// Array of zeros that would read as "every instruction is at offset 0".
Value primCodeDescriptors(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	CodeUnit *unit = unitOf(primitiveReceiver(args));
	if (unit == NULL) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	// Re-read: allocating the Array can move nothing the unit names (a unit is
	// C memory), but the receiver's unit pointer is read again for the count.
	uint16_t count = unit->sourcePositions == NULL ? 0 : unit->instructionCount;
	Array *array = newArray(count);
	// The unit is re-read through the receiver because the allocation above may
	// have moved the receiver, and the stores are plain: a source position is a
	// SmallInteger, and an immediate never needs the write barrier.
	unit = unitOf(primitiveReceiver(args));
	for (uint16_t i = 0; i < count; i++) {
		array->raw->vars[i] = tagInt((intptr_t) unit->sourcePositions[i]);
	}
	Value answer = objectTagged(array);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Block>>compiledBlock -- the code object a closure runs.
Value primBlockCode(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER) || Handles.Closure.raw == NULL
			|| rawObjectClassIndex(asObject(receiver))
				!= classIndexOf(&Handles.Closure)) {
		return PRIMITIVE_FAILED;
	}
	return ((RawClosure *) asObject(receiver))->method;
}
