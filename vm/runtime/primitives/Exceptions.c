// Exceptions.
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
//
// It is also why this file cannot be folded into a helper shared with Block.c
// even though both enter a closure: the setjmp has to be HERE.

#include "runtime/primitives/Shared.h"


// Block>>basicOn: anExceptionClass do: aHandlerBlock
Value primBlockOnException(Value *args, uint64_t argc)
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
Value primExceptionSignal(Value *args, uint64_t argc)
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
Value primBlockUnwindProtected(Value *args, uint64_t argc)
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
