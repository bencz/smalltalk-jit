#ifndef SCHEDULER_H
#define SCHEDULER_H

// The cooperative scheduler: which fiber runs, and when the others get a turn.
//
// concurrency/Fiber.c owns the machine-level switch and the stacks; this owns
// the POLICY. The split is the one Fiber.h already anticipated -- it carries a
// FiberState, a ParkIntent and a FiberRoots for exactly this layer to drive.
//
// COOPERATIVE AND SINGLE-THREADED, on purpose and stated rather than implied.
// A fiber runs until it yields, sleeps, suspends or finishes; nothing preempts
// it. That is what `Processor yield` means in the kernel, it is what the actor
// and HTTP code is written against, and it is the model the old VM grew workers
// on top of later. Workers are not here yet and this file does not pretend
// they are.
//
// WHAT MAKES THIS DIFFERENT FROM A COROUTINE LIBRARY is the collector. A parked
// fiber holds Smalltalk objects in three places at once -- its FiberRoots, its
// native frames, and its entry block -- and none of them is reachable from any
// Thread. rootsVisitFibers (memory/Roots.h) is what closes that, and it is the
// reason this file provides a root visitor at all.

#include "core/Object.h"
#include "concurrency/Fiber.h"

// Bind the OS thread's own stack to a Fiber so it can be switched away from
// like any other, and make it the running one. Idempotent; every entry point
// below calls it, so nothing has to remember to.
void schedulerInit(void);

// Create a SUSPENDED fiber that will evaluate `block`, and answer its id. It
// does not run until it is resumed, which is what `Block>>newProcess` promises
// and what makes `fork` = spawn + resume.
//
// Answers 0 when the block is not a closure or a stack cannot be reserved; no
// live fiber ever has id 0.
size_t schedulerSpawn(Value block);

// The id of the fiber running right now. The main fiber is id 1.
size_t schedulerCurrentId(void);

// Give other ready fibers a turn. Returns when this one is scheduled again;
// with nothing else ready it returns immediately, having done nothing.
void schedulerYield(void);

// Park the current fiber off the ready queue. It runs again only when some
// other fiber resumes it by id, so a fiber that suspends with nobody holding
// its id is gone for good -- which is the caller's business, not this file's.
void schedulerSuspendCurrent(void);

// Park the current fiber until at least `micros` microseconds have passed.
void schedulerSleep(int64_t micros);

// Make a SUSPENDED fiber ready. Answers 0 when no fiber has that id, or when it
// is not suspended: resuming a running or ready fiber is a no-op and answering
// false is how the caller can tell.
_Bool schedulerResume(size_t id);

// Terminate a fiber, answering whether it is going to die. Terminating the
// RUNNING one NEVER ANSWERS: it unwinds this stack and leaves through the
// fiber's exit record (jit/Jit.h). Terminating another marks it and makes it
// runnable, and it dies the next time it has the CPU, on its own stack, because
// that is the only place its `ensure:` blocks can run.
_Bool schedulerTerminate(size_t id);

// ---- the sync monitor ------------------------------------------------------
//
// What Semaphore, Channel, SharedDictionary and ConcurrentDictionary are built
// on: a critical section, plus a park that releases it.
//
// ONE MONITOR, not a striped set. The old VM striped by sync object because it
// ran many OS threads and unrelated objects serializing on one lock was a
// measured bottleneck (docs, monitor-sharding). This scheduler is cooperative
// and single-threaded (top of this file), so there is no parallelism for
// stripes to recover: a critical section here is never CONTENDED, only ever
// re-entered or parked out of. `monitorEnterOn:` therefore takes its object and
// ignores it, and the day workers arrive the stripe comes back as an
// implementation detail behind these three calls.
//
// EACH ANSWERS 0 RATHER THAN ASSERTING when the caller is out of protocol --
// entering twice, exiting without holding, parking without holding. The kernel
// declares all four with empty bodies, so a 0 becomes `self primitiveFailed:`,
// which names the primitive and can be caught. Aborting the VM for a Smalltalk
// program's mistake is the other option and it is worse.
_Bool schedulerMonitorEnter(void);
_Bool schedulerMonitorExit(void);
// Release the monitor and park, in that order and with nothing in between.
// Atomic for free here: no switch point separates the two, so a would-be waker
// cannot run in the window that a preemptive scheduler has to close explicitly.
_Bool schedulerMonitorPark(void);

#endif
