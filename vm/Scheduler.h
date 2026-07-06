#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "Fiber.h"
#include "Object.h"
#include <stddef.h>
#include <stdint.h>

// ---- lifecycle -----------------------------------------------------------

// Activate the scheduler. The calling OS-thread stack becomes the scheduler
// context (where the run loop and, later, the event loop execute). Must be
// called after bootstrap/snapshot load and before any fiber runs.
void schedulerInit(void);

// Create a fiber that runs a C entry point (used for the top-level program
// fiber) and schedule it. Returns the fiber.
Fiber *schedulerSpawnC(FiberCEntry entry, void *arg, size_t stackSize);

// Run the scheduler loop on the current (scheduler) stack until there is no
// runnable or pending work left. Returns the recorded exit result.
Value schedulerRun(void);

// ---- cross-isolate inbox ------------------------------------------------

// Register `eventFd` (an isolate's inbox eventfd) in this scheduler's epoll set.
// While an inbox is registered the run loop keeps blocking on events (it does not
// exit for lack of local work), and when the fd fires it calls isolateDrainInbox.
void schedulerRegisterInboxFd(int eventFd);

// Stop keeping the loop alive for the inbox (lets schedulerRun exit once local
// work drains). The caller should also poke the eventfd to wake the loop.
void schedulerStopInbox(void);

// ---- process operations (called from primitives, on a fiber) -------------

// Create a fiber that will evaluate `block`, SUSPENDED (not scheduled).
// Returns its stable id.
size_t schedulerSpawnBlock(Value block);

// Move a suspended/ready fiber into the ready queue.
void schedulerResume(size_t id);

// Cooperatively give up the CPU; the current fiber goes to the back of the
// ready queue. Returns when it is scheduled again.
void schedulerYield(void);

// Park the current fiber WITHOUT re-queuing it. It stays off the run queue
// until something calls schedulerResume() on its id. Building block for
// semaphores, channels and other blocking synchronisation.
void schedulerSuspend(void);

// Park the current fiber until at least `micros` microseconds have elapsed.
void schedulerSleep(int64_t micros);

// Park the current fiber until `fd` is ready for reading (forWrite == 0) or
// writing (forWrite != 0). This is how non-blocking socket I/O turns into
// linear, blocking-looking code without stalling the whole VM.
void schedulerWaitFd(int fd, int forWrite);

// Terminate a fiber by id. If it is the current fiber this never returns.
void schedulerTerminate(size_t id);

// Id of the currently running fiber (0-based; valid only inside a fiber).
size_t schedulerCurrentId(void);

// Entry point invoked (on the new fiber's own stack) by fiberTrampoline.
void schedulerFiberMain(void);

// ---- GC integration ------------------------------------------------------

_Bool schedulerActive(void);
size_t schedulerFiberSlots(void);         // upper bound for iteration
Fiber *schedulerFiberAt(size_t slot);     // may be NULL (freed slot)

// Copy the running fiber's live roots into its record before a GC walk, and
// copy them back afterwards (so forwarded context/handler pointers stay live).
void schedulerSyncCurrentRoots(void);
void schedulerRestoreCurrentRoots(void);

#endif
