// Processes: what the kernel's ProcessorScheduler and Block>>fork sit on.
//
// A Smalltalk Process IS a fiber (concurrency/Fiber.h), and the kernel holds
// one only by ID -- `Process` has a single instance variable, `id`. That
// indirection is not incidental: a fiber is C memory the collector does not
// move and does not own, so handing Smalltalk a pointer to one would be a
// reference the image could keep alive after the fiber was reclaimed. An id
// that no longer resolves is an ordinary answer of false.
//
// NONE OF THESE IS A SEND THAT RETURNS NORMALLY. `yield`, `suspendCurrent` and
// `sleepMicroseconds:` all switch stacks in the middle, and come back when
// somebody schedules this fiber again -- possibly after a collection has moved
// every object the caller was holding. Everything they answer is therefore an
// immediate, and they anchor the calling frame first, because the fibers that
// run in between allocate.

#include "runtime/primitives/Shared.h"
#include "concurrency/Scheduler.h"

// Block>>basicSpawn: a SUSPENDED process evaluating this block, answered as its
// id. `Block>>fork` is this plus resume, which is why nothing starts here.
Value primProcessSpawn(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value block = primitiveReceiver(args);
	if (!isClosure(block)) {
		return PRIMITIVE_FAILED;
	}
	// Reserving a stack does not allocate on the Smalltalk heap, but the fiber
	// keeps the block, so the frame is anchored: nothing may collect between
	// storing the block in C memory the collector cannot yet see and the
	// registry entry that makes it visible.
	PRIMITIVE_ALLOCATES(args);
	size_t id = schedulerSpawn(block);
	PRIMITIVE_DONE_ALLOCATING();
	return id == 0 ? PRIMITIVE_FAILED : tagInt((intptr_t) id);
}


Value primProcessCurrentId(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) schedulerCurrentId());
}


Value primProcessYield(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	schedulerYield();
	PRIMITIVE_DONE_ALLOCATING();
	// The receiver is re-read AFTER the switch: other fibers ran while this one
	// was parked, and any of them may have triggered a collection that moved it.
	return primitiveReceiver(args);
}


Value primProcessSuspend(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	schedulerSuspendCurrent();
	PRIMITIVE_DONE_ALLOCATING();
	return primitiveReceiver(args);
}


Value primProcessSleep(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value micros = primitiveArgument(args, 0);
	if (!valueTypeOf(micros, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	schedulerSleep((int64_t) asCInt(micros));
	PRIMITIVE_DONE_ALLOCATING();
	return primitiveReceiver(args);
}


// Answers false for an id that names nothing, or a process that was not
// suspended. Not a failure: "that process is not waiting" is an answer, and the
// kernel's `Process>>resume` is written to take one.
Value primProcessResume(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value id = primitiveArgument(args, 0);
	if (!valueTypeOf(id, VALUE_INT) || asCInt(id) < 0) {
		return PRIMITIVE_FAILED;
	}
	// Resuming only moves a fiber onto the ready queue; it does not switch, so
	// nothing runs here and nothing can collect.
	return booleanResult(schedulerResume((size_t) asCInt(id)));
}


// ---------------------------------------------------------------------------
// The sync monitor
// ---------------------------------------------------------------------------
//
// Four primitives over three scheduler calls. The reasoning about why there is
// ONE monitor rather than a striped set, and why each answers rather than
// asserting when the caller is out of protocol, is at their declarations in
// concurrency/Scheduler.h.
//
// A FAILURE HERE IS THE POINT. All four kernel methods have empty bodies, so a
// failed attempt becomes `self primitiveFailed: #MonitorEnterPrimitive` and
// names itself. Entering twice, exiting without holding and parking without
// holding are all program errors in the Smalltalk, and they say so.

// ProcessorScheduler>>monitorEnter
Value primMonitorEnter(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// Entering can YIELD when another fiber holds the monitor, and a yield runs
	// other fibers, which allocate. So the frame is anchored exactly as the
	// other switching primitives above anchor theirs.
	PRIMITIVE_ALLOCATES(args);
	_Bool taken = schedulerMonitorEnter();
	PRIMITIVE_DONE_ALLOCATING();
	return taken ? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


// ProcessorScheduler>>monitorEnterOn: anObject
//
// THE OBJECT IS ACCEPTED AND IGNORED, and that is not a shortcut. It names the
// stripe in a striped monitor, and there is one monitor here because there is no
// parallelism to stripe (Scheduler.h). The kernel keeps sending it because the
// day workers arrive it is the argument that makes striping possible again
// without touching a single caller.
Value primMonitorEnterOn(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	_Bool taken = schedulerMonitorEnter();
	PRIMITIVE_DONE_ALLOCATING();
	return taken ? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


// ProcessorScheduler>>monitorExit
Value primMonitorExit(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// Releasing switches nothing and allocates nothing.
	return schedulerMonitorExit() ? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


// ProcessorScheduler>>parkOnMonitor
Value primMonitorPark(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	_Bool parked = schedulerMonitorPark();
	PRIMITIVE_DONE_ALLOCATING();
	// The receiver is re-read AFTER the park: other fibers ran while this one
	// was off the queue, and any of them may have collected.
	return parked ? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


Value primProcessTerminate(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value id = primitiveArgument(args, 0);
	if (!valueTypeOf(id, VALUE_INT) || asCInt(id) < 0) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(schedulerTerminate((size_t) asCInt(id)));
}
