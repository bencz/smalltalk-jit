#ifndef FIBER_H
#define FIBER_H

#include "Object.h"
#include <stddef.h>

struct EntryStackFrame;
struct Handle;
struct HandleScope;

typedef enum {
	FIBER_READY,      // in the ready queue, waiting to run
	FIBER_RUNNING,    // currently executing
	FIBER_SUSPENDED,  // parked (semaphore/timer/io), not in ready queue
	FIBER_DONE        // finished, awaiting destruction
} FiberState;

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
	void *stackBase;        // mmap base (guard page here); NULL for the scheduler ctx
	size_t stackSize;       // total mapped size including guard page

	FiberState state;
	FiberRoots roots;

	Value entryBlock;       // Smalltalk block to run (nil for C-entry fibers), a GC root
	Value process;          // the Smalltalk Process object, a GC root
	FiberCEntry cEntry;     // C entry point (used for the main/program fiber)
	void *cArg;

	size_t id;              // stable id, index into the scheduler fiber registry
	int waitFd;             // fd this fiber is EPOLLONESHOT-armed on, or -1
	struct Fiber *queueNext; // intrusive link for the ready queue / wait queues
} Fiber;

// Low-level context switch (defined in Fiber.c via inline asm).
// Saves the current callee-saved regs + rsp into *saveSp, then loads newSp.
void fiberSwitchAsm(void **saveSp, void *newSp);

// Allocate a fiber with its own stack, primed so that the first switch into it
// begins executing fiberTrampoline (which calls fiberMain). Does not schedule it.
Fiber *fiberCreate(size_t stackSize);
void fiberDestroy(Fiber *fiber);

// The trampoline the freshly-created stack "returns" into on first switch.
void fiberTrampoline(void);

#endif
