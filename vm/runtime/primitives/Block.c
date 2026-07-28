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


// ---------------------------------------------------------------------------
// Looping on a block that is NOT a literal
// ---------------------------------------------------------------------------
//
// `[...] whileTrue: [...]` written literally never reaches here: the front end
// inlines it into jumps, which is the whole reason ADR 0006 lets it (there is
// no loop to optimize otherwise). What reaches here is the form where the block
// arrived in a VARIABLE, so the compiler cannot see it -- `b := [...]. b
// whileTrue` -- and that form has no other implementation.
//
// The two arms are the same loop; only the body differs, so they share one.
//
// A CONDITION THAT IS NOT A BOOLEAN FAILS, and the kernel's fallback is what
// signals `must return a Boolean`. Treating a non-boolean as false would turn
// `[1] whileTrue` into a silent no-op, and there are tests that require it to
// raise.

static Value whileLoop(Value *args, _Bool withBody)
{
	Value condition = primitiveReceiver(args);
	if (!isClosure(condition)) {
		return PRIMITIVE_FAILED;
	}
	if (withBody && !isClosure(primitiveArgument(args, 0))) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	for (;;) {
		// RE-READ EVERY ITERATION. The body allocates, so a collection between
		// two turns of this loop moves both blocks; a Value cached before the
		// loop would name whichever address they had at the start.
		_Bool understood = 0;
		Value answer = jitSendUnary(primitiveReceiver(args), "value", &understood);
		if (!understood) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		if (!valueTypeOf(answer, VALUE_POINTER)) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED; // an immediate is not a Boolean
		}
		RawObject *object = asObject(answer);
		if (object == Handles.false_.raw) {
			break;
		}
		if (object != Handles.true_.raw) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		if (withBody) {
			jitSendUnary(primitiveArgument(args, 0), "value", &understood);
			if (!understood) {
				PRIMITIVE_DONE_ALLOCATING();
				return PRIMITIVE_FAILED;
			}
		}
	}
	// THE RECEIVER, re-read once more, because the loop just allocated. Both
	// kernel methods answer it (`block whileTrue == block` is a test).
	Value receiver = primitiveReceiver(args);
	PRIMITIVE_DONE_ALLOCATING();
	return receiver;
}


Value primBlockWhileTrue(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	return whileLoop(args, 0);
}


Value primBlockWhileTrue2(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	return whileLoop(args, 1);
}
