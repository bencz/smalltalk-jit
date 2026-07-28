// Memory ordering.
//
// WHAT IS HERE IS ONE FENCE, and the rest of the Atomic family
// (AtomicLoad/Store/CompareAndSet/GetAndAdd/GetAndSet and the AtomicArray five)
// is still PRIMITIVE_ABSENT. That is not a stopping point chosen at random: a
// fence is meaningful in a single-threaded scheduler and an atomic cell is not
// yet, because there is nothing to be atomic against.
//
// THE FENCE STILL HAS TO EXIST, and the reason is visible rather than
// theoretical. ConcurrentDictionary publishes a copy-on-write snapshot:
//
//     copy := self copyOfSnapshot.
//     copy at: key put: anObject.
//     Atomic releaseFence.
//     snapshot := copy.
//
// With the primitive absent, `releaseFence` raised INSIDE the critical section,
// so `Processor monitorExit` below it never ran, the monitor stayed held, and
// the next write from the same fiber failed as a re-entry. One missing fence
// took out every write path in the class, and the error it produced named the
// monitor rather than the fence.

#include "runtime/primitives/Shared.h"

// Atomic class>>releaseFence
//
// A COMPILER AND CPU BARRIER, and on this scheduler the first is the part that
// does the work: fibers are cooperative and single-threaded (concurrency/
// Scheduler.h), so no other mutator can observe the store it orders. It is
// still emitted rather than made a no-op, because what it orders is a
// PUBLICATION -- the fields of a freshly built object against the pointer that
// makes it visible -- and that ordering is the same requirement the day workers
// arrive. A no-op today would be a no-op nobody remembered to fill in then.
//
// Nothing here allocates or switches, so there is no frame to anchor.
Value primMemoryReleaseFence(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	return primitiveReceiver(args);
}
