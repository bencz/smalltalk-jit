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
#include "runtime/Collection.h"
#include <stdlib.h>


// The closure's compiled code, or NULL when the receiver is not a closure, its
// arity does not match, or the tier cannot compile it.
static RawCompiledMethod *runnableClosure(Value receiver, uint64_t argc)
{
	if (!isClosure(receiver)) {
		return NULL;
	}
	RawClosure *closure = (RawClosure *) asObject(receiver);
	RawCompiledMethod *method = (RawCompiledMethod *) asObject(closure->method);
	if (method->unit->argumentCount != argc) {
		return NULL; // wrong number of arguments for this block
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


// Enter a closure with `count` arguments, given ASCENDING in `values`.
//
// ONE PLACE THAT KNOWS BOTH CALLING CONVENTIONS, so `value:`, `value:value:`
// and `valueWithArguments:` cannot drift apart on which one a given block uses.
// A block whose arity passes the ABI's argument registers is compiled WIDE and
// takes a pointer to a descending argument block instead (jit/Jit.h); the
// closure itself is register 0 of the block's frame either way, which is how
// GETUP reaches the captured values with one load.
//
// NOTHING ALLOCATES between laying out that block and the call, which is what
// makes a malloc'd buffer of bare Values safe here: the callee's prologue copies
// them into its own frame slots before any Smalltalk runs.
static Value enterClosureWith(Value receiver, RawCompiledMethod *method,
	const Value *values, uint64_t count)
{
	if (method->native->wide) {
		Value *block = malloc((size_t) (count + 1) * sizeof(Value));
		if (block == NULL) {
			return PRIMITIVE_FAILED;
		}
		block[count] = receiver; // the receiver is HIGHEST, arguments descend
		for (uint64_t i = 0; i < count; i++) {
			block[count - 1 - i] = values[i];
		}
		Value answer = jitCallWide(method->native, &block[count]);
		free(block);
		return answer;
	}
	switch (count) {
	case 0: return jitCall0(method->native, receiver);
	case 1: return jitCall1(method->native, receiver, values[0]);
	case 2: return jitCall2(method->native, receiver, values[0], values[1]);
	case 3: return jitCall3(method->native, receiver, values[0], values[1], values[2]);
	case 4: return jitCall4(method->native, receiver, values[0], values[1], values[2],
		values[3]);
	default: return jitCall5(method->native, receiver, values[0], values[1], values[2],
		values[3], values[4]);
	}
}


// `value`, `value:`, `value:value:` and `value:value:value:`: the arguments are
// in this primitive's own frame slots.
static Value enterClosure(Value *args, uint64_t argc)
{
	RawCompiledMethod *method = runnableClosure(primitiveReceiver(args), argc);
	if (method == NULL) {
		return PRIMITIVE_FAILED;
	}
	// READ OUT OF THE FRAME AFTER compiling and with nothing allocating after:
	// a frame slot is a root the collector updates, a copy of one is not.
	Value values[JIT_MAX_NARROW_ARGS];
	for (uint64_t i = 0; i < argc && i < JIT_MAX_NARROW_ARGS; i++) {
		values[i] = primitiveArgument(args, i);
	}
	return enterClosureWith(primitiveReceiver(args), method, values, argc);
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


Value primClosureValue3(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = enterClosure(args, 3);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Block>>valueWithArguments: anArray
//
// NO ARITY CEILING, because the arguments never become C arguments: a block
// past the register set is entered through the wide convention, which takes one
// pointer however many there are. The positional `value:` family stops at three
// because that is where the kernel's selectors stop, not because of the ABI.
Value primClosureValueArgs(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value arguments = primitiveArgument(args, 0);
	if (!valueTypeOf(arguments, VALUE_POINTER)
			|| rawObjectFormat(asObject(arguments)) != FORMAT_INDEXED_POINTERS) {
		return PRIMITIVE_FAILED;
	}
	size_t count = rawObjectElementCount(asObject(arguments));
	PRIMITIVE_ALLOCATES(args);
	RawCompiledMethod *method = runnableClosure(primitiveReceiver(args), count);
	if (method == NULL) {
		PRIMITIVE_DONE_ALLOCATING();
		return PRIMITIVE_FAILED; // wrong arity: the kernel's fallback raises
	}
	// The Array is re-read HERE, after compiling, and copied out with nothing
	// allocating before the call: entering the block allocates, and an Array
	// address taken earlier would be one a collection may have moved.
	Value *values = malloc((count == 0 ? 1 : count) * sizeof(Value));
	if (values == NULL) {
		PRIMITIVE_DONE_ALLOCATING();
		return PRIMITIVE_FAILED;
	}
	RawArray *array = (RawArray *) asObject(primitiveArgument(args, 0));
	for (size_t i = 0; i < count; i++) {
		values[i] = array->vars[i];
	}
	Value answer = enterClosureWith(primitiveReceiver(args), method, values, count);
	free(values);
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
