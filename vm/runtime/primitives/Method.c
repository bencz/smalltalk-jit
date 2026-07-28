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
