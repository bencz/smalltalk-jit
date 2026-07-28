// Entering a block.
//
// `aBlock value` is a SEND, exactly like everything else, so the site carries an
// inline cache and the optimizer can see which block actually runs there. What
// the primitive does is enter the closure's own compiled code with the CLOSURE
// as receiver: register 0 of a block's frame is the closure itself, which is how
// GETUP reaches the captured values with one load.
//
// All three anchor, because the block's own body allocates as freely as any
// method.

#include "runtime/primitives/Shared.h"
#include "jit/CompiledMethod.h"
#include "runtime/Closure.h"


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


Value primClosureValue(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 0);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primClosureValue1(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 1);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


Value primClosureValue2(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 2);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
