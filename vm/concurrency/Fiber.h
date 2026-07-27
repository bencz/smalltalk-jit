#ifndef FIBER_H
#define FIBER_H

// Stackful coroutines: each fiber owns a real machine stack, so a Smalltalk
// activation can be suspended anywhere without the VM having to reify it.
//
// Three properties of the old design are kept, and each for a reason that was
// established rather than assumed:
//
//   * THE SWITCH ITSELF. Save the ABI's callee-saved set, swap the stack
//     pointer, restore, return. Twenty instructions, forced by the ABI. There
//     is no better version, only wrong ones.
//   * FIBERS ARE PINNED to one worker. This was not a preference: migrating a
//     fiber between OS threads corrupted the C unwinder, which was root-caused
//     and closed by pinning.
//   * STACKS GROW IN PLACE. The region is reserved untouched and only a window
//     at the high end is committed; a fault below it grows the window down.
//     100k fibers at a fixed 1MB each is not a thing you can map.
//
// What is NEW is the connection to the collector. A fiber's stack is where
// native frames live, and every one of them has to be walkable with an EXACT
// typed map (memory/Roots.h): pointer, raw f64, raw i64, dead. The old design
// had a bitset of "slots to scan" and a collector that silently repaired what
// disagreed with it. Nothing here repairs anything.

#include "core/Object.h"
#include "memory/Roots.h"
#include <stddef.h>

struct EntryStackFrame;
struct HandleScope;
struct Heap;

typedef enum {
	FIBER_READY,      // runnable, waiting its turn
	FIBER_RUNNING,    // executing on its worker right now
	FIBER_PARKING,    // chose to suspend, has not yet switched off its stack
	FIBER_SUSPENDED,  // switched off, not runnable
	FIBER_DONE,       // returned from its entry, stack reclaimable
} FiberState;

// What the run loop must do once a FIBER_PARKING fiber has fully switched off
// its worker's stack. The commit happens AFTER the switch, on the scheduler's
// own stack, so a peer can never pop and run a stack that is still switching.
typedef enum {
	PARK_NONE = 0,
	PARK_YIELD,   // back of the ready queue
	PARK_SUSPEND, // off the ready queue entirely
} ParkIntent;

// The slice of VM execution state that belongs to a fiber rather than to the
// OS thread. These hold the live values while the fiber runs and the saved ones
// while it is suspended, and they are roots in BOTH cases.
typedef struct FiberRoots {
	struct EntryStackFrame *stackFramesTail; // native frame chain, the deep root set
	struct HandleScope *handleScopes;
	Value context;
	Value exceptionHandler;
	Value unwindHandler;
} FiberRoots;

typedef void (*FiberEntry)(void *arg);

typedef struct Fiber {
	// Saved stack pointer while not running. The FIRST field on purpose: the
	// switch reaches it with a zero displacement.
	void *sp;

	// The reservation, and the committed window inside it. `stackHigh` is the
	// highest usable byte plus one; the window is [committedLow, stackHigh) and
	// grows DOWNWARD. A fault below `floor` is a genuine overflow.
	uint8_t *reservation;
	size_t reservationSize;
	uint8_t *stackHigh;
	// VOLATILE, and this is load-bearing rather than defensive: the growth
	// handler writes this field from a SIGNAL, asynchronously, while ordinary
	// code is running. Without the qualifier the compiler is entitled to hoist
	// the load out of any loop or call that "cannot" modify it, and then a
	// reader sees the window it had before the stack grew. That is not a
	// theoretical hazard: it is exactly what the fiber self-test caught, with
	// a stack that had visibly grown to 488 KB still being reported as 16 KB.
	uint8_t *volatile committedLow;
	uint8_t *floor;

	FiberState state;
	ParkIntent parkIntent;
	_Bool parkPending; // a wake raced in while this fiber was still PARKING
	int homeWorker;    // the ONE worker this fiber runs on; fibers do not migrate

	FiberRoots roots;

	Value entryBlock; // Smalltalk block to run, or nil for a C-entry fiber
	Value process;    // the Smalltalk Process object
	FiberEntry cEntry;
	void *cArg;

	size_t id;
	struct Fiber *queueNext; // ready/wait queue link, valid only when not running
	// Who switched INTO this fiber. The trampoline of a finished fiber needs it
	// to hand control back, and it is not the same as queueNext: a fiber can be
	// resumed by a peer it never yielded to.
	struct Fiber *resumer;
	// A fiber is DIRTY from the moment it runs until a collection finds all of
	// its direct roots old. Only dirty fibers need walking; the rest are covered
	// by the remembered set.
	_Bool dirty;
	struct Fiber *dirtyNext;
	struct Fiber *dirtyPrev;
} Fiber;

// ---- the machine-level switch, one file per CPU ---------------------------
//
// Save the current context's callee-saved registers on its own stack, store the
// resulting sp through `saveSp`, switch to `newSp`, restore that context's
// registers and return into it.
void fiberSwitchAsm(void **saveSp, void *newSp);

// Prime a fresh stack so that the FIRST fiberSwitchAsm into the returned sp
// pops a zeroed callee-saved frame and returns into `entry`, with whatever
// entry alignment the ABI requires. `top` is the highest usable address and at
// least one page below it is committed.
void *fiberPrimeStack(void *top, void (*entry)(void));

// ---- lifecycle -------------------------------------------------------------

// Call once per OS thread before creating any fiber: caches the page size and
// sets how much of a new stack is committed up front.
void fiberInitStacks(size_t reservationBytes, size_t initialCommitBytes);

Fiber *fiberCreate(FiberEntry entry, void *arg);
// Bind this OS thread's own context to a Fiber, so the scheduler can switch
// away from it with the same mechanism as everything else.
void fiberAdoptCurrentStack(Fiber *fiber);
// The fiber this OS thread is running right now.
Fiber *fiberCurrent(void);
void fiberDestroy(Fiber *fiber);
// Hand the untouched part of a suspended fiber's stack back to the OS.
void fiberReleaseIdleStack(Fiber *fiber);

// Switch from `from` to `to`. Returns when someone switches back into `from`.
void fiberSwitch(Fiber *from, Fiber *to);

// Grow `fiber`'s committed window down to cover `faultAddress`. Returns 1 when
// it grew (retry the faulting instruction), 0 when the address is outside the
// growable window, which is a genuine fault and must not be swallowed.
int fiberGrowStack(Fiber *fiber, uintptr_t faultAddress);

// Install the fault handler that turns a stack-growth fault into a grow. One
// per process.
void fiberInstallGrowthHandler(void);

// Is `address` inside this fiber's COMMITTED stack window? The collector uses
// this to bound a frame walk: a frame pointer outside the window is not a
// frame, and finding one is a bug in the frame chain, not something to skip.
static inline _Bool fiberStackContains(Fiber *fiber, void *address)
{
	uint8_t *p = address;
	return fiber->committedLow <= p && p < fiber->stackHigh;
}

// Walk one fiber's roots: its FiberRoots slots, then its native frames through
// the engine's rootsVisitNativeFrames.
void fiberVisitRoots(Fiber *fiber, RootVisitor visit, void *ctx);

#endif
