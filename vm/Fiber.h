#ifndef FIBER_H
#define FIBER_H

#include "Object.h"
#include <stddef.h>

// ThreadSanitizer cannot follow user-level fiber context switches on its own: a fiber's
// stack migrates across OS threads (park on one worker, resume on another), which
// confuses TSan's per-OS-thread shadow stack and crashes it. TSan's fiber API annotates
// each switch so it tracks the running stack correctly. Zero-cost in a normal build.
#ifdef __SANITIZE_THREAD__
#include <sanitizer/tsan_interface.h>
#define TSAN_CREATE_FIBER()      __tsan_create_fiber(0)
#define TSAN_DESTROY_FIBER(f)    __tsan_destroy_fiber(f)
#define TSAN_SWITCH_TO_FIBER(f)  __tsan_switch_to_fiber((f), 0)
#define TSAN_CURRENT_FIBER()     __tsan_get_current_fiber()
#else
#define TSAN_CREATE_FIBER()      NULL
#define TSAN_DESTROY_FIBER(f)    ((void) (f))
#define TSAN_SWITCH_TO_FIBER(f)  ((void) (f))
#define TSAN_CURRENT_FIBER()     NULL
#endif

struct EntryStackFrame;
struct Handle;
struct HandleScope;

typedef enum {
	FIBER_READY,      // in the ready queue, waiting to run
	FIBER_RUNNING,    // currently executing on some worker
	FIBER_PARKING,    // transient: chose to yield/suspend, not yet switched off its
	                  // worker's stack. NEVER observed at a GC safepoint (the park
	                  // path is allocation-free), so it is not a scan state.
	FIBER_SUSPENDED,  // parked (semaphore/timer/io), not in ready queue
	FIBER_DONE        // finished, awaiting destruction
} FiberState;

// What the scheduler run loop must do once a FIBER_PARKING fiber has fully switched
// off its worker's stack (the "commit" happens on the scheduler stack, after the
// context switch, so a peer worker can never pop-and-run the same stack mid-switch).
typedef enum {
	PARK_NONE = 0,
	PARK_YIELD,    // re-queue at the back of the ready queue
	PARK_SUSPEND   // park off the ready queue (unless a resume raced in: parkPending)
} ParkIntent;

// The subset of VM execution state that is private to each fiber. The shared
// isolate state (heap, persistent handle list) lives in CurrentThread and is
// NOT duplicated here. These fields are kept in sync with CurrentThread /
// CurrentExceptionHandler while the fiber runs; they hold the saved values
// while it is suspended, and are the roots the GC walks for every fiber.
typedef struct FiberRoots {
	struct EntryStackFrame *stackFramesTail;
	struct HandleScope *handleScopes;
	Value context;
	Value exceptionHandler;
} FiberRoots;

typedef void (*FiberCEntry)(void *arg);

typedef struct Fiber {
	void *sp;               // saved stack pointer while not running
	void *stackBase;        // mmap base (low end); NULL for the scheduler ctx
	size_t stackSize;       // total mapped (reserved) size incl. the floor page
	// In-place growable stack: the region is reserved PROT_NONE and only a small
	// window at the high end is committed RW. `committedLow` is that window's low
	// edge (moves DOWN as the SIGSEGV handler grows it); `reserveFloor` is the hard
	// limit below which a fault is a genuine overflow, not a grow.
	void *committedLow;
	void *reserveFloor;

	FiberState state;
	// Park-handoff (multi-worker): set by the fiber under the scheduler lock before it
	// switches off; read by the run loop's commit after the switch. `parkPending` is a
	// wake that raced in while the fiber was still FIBER_PARKING — the commit re-queues
	// instead of parking, so no wakeup is ever lost.
	ParkIntent parkIntent;
	_Bool parkPending;
	FiberRoots roots;

	Value entryBlock;       // Smalltalk block to run (nil for C-entry fibers), a GC root
	Value process;          // the Smalltalk Process object, a GC root
	FiberCEntry cEntry;     // C entry point (used for the main/program fiber)
	void *cArg;

	size_t id;              // stable id, index into the scheduler fiber registry
	int waitFd;             // fd this fiber is EPOLLONESHOT-armed on, or -1
	struct Fiber *queueNext; // intrusive link for the ready queue / wait queues
	// GC: a fiber is "dirty" (may hold a young root directly on its stack/handles)
	// from when it is registered/run until a scavenge finds all its direct roots
	// old. Only dirty fibers are walked by the scavenger; clean ones are covered
	// by the remembered set. Linked into the scheduler's dirty list.
	_Bool dirty;
	struct Fiber *dirtyNext;
	struct Fiber *dirtyPrev;
	void *tsanFiber;        // TSan fiber handle (NULL in a normal build; see Fiber.h top)
} Fiber;

// Low-level context switch (defined in Fiber.c via inline asm).
// Saves the current callee-saved regs + rsp into *saveSp, then loads newSp.
void fiberSwitchAsm(void **saveSp, void *newSp);

// Allocate a fiber with its own stack, primed so that the first switch into it
// begins executing fiberTrampoline (which calls fiberMain). Does not schedule it.
Fiber *fiberCreate(size_t stackSize);
void fiberDestroy(Fiber *fiber);
void fiberReleaseIdleStack(Fiber *fiber); // madvise the dead stack region on park
// Set the initial committed window (bytes) for new fiber stacks + cache the page
// size. Call once per OS thread from schedulerInit before any fiber is created.
void fiberInitStackGrowth(size_t initialCommitBytes);
// Grow `fiber`'s committed window down to cover `faultAddr` (called from the
// SIGSEGV handler). Returns 1 if it grew (retry the instruction), 0 if the fault
// is outside the growable window / below the floor (a genuine fault).
int fiberGrowStack(Fiber *fiber, uintptr_t faultAddr);

// The trampoline the freshly-created stack "returns" into on first switch.
void fiberTrampoline(void);

#endif
