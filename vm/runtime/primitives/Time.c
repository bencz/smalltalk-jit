// Time, and the heap's own numbers.

#include "runtime/primitives/Shared.h"
#include "os/Os.h"


Value primMonotonicNanos(Value *args, uint64_t argc)
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


Value primCurrentMicroTime(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	int64_t micros = osCurrentMicroTime();
	return smallIntFits(micros) ? tagInt((intptr_t) micros) : PRIMITIVE_FAILED;
}


Value primCollectGarbage(Value *args, uint64_t argc)
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


Value primPrintHeap(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	printHeap(CurrentThread.heap);
	return primitiveReceiver(args);
}
