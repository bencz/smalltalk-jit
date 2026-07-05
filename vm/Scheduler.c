#include "Scheduler.h"
#include "Thread.h"
#include "Heap.h"
#include "Handle.h"
#include "Entry.h"
#include "Smalltalk.h"
#include "Exception.h"
#include "StackFrame.h"
#include "Os.h"
#include "Assert.h"
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>

// ---------------------------------------------------------------------------
// Cooperative single-OS-thread fiber scheduler.
//
// The original OS-thread stack is the "scheduler context": schedulerRun() loops
// there, switching into ready fibers. A fiber returns control by switching back
// to the scheduler (yield / block / finish). Because only one fiber runs at a
// time and the heap is never touched by the scheduler itself, the GC and all VM
// data structures stay single-threaded and lock-free.
// ---------------------------------------------------------------------------

static Fiber gScheduler;          // scheduler context (stackBase == NULL)
static Fiber *gCurrent = NULL;    // running fiber, NULL while in the scheduler
static _Bool gActive = 0;
static Value gExitResult = 0;

// ready queue (intrusive FIFO through Fiber.queueNext)
static Fiber *gReadyHead = NULL;
static Fiber *gReadyTail = NULL;

// timer min-heap: fibers sleeping until a deadline (Delay wait)
typedef struct {
	int64_t deadline; // absolute microseconds
	size_t fiberId;
} Timer;
static Timer *gTimers = NULL;
static size_t gTimerCount = 0;
static size_t gTimerCap = 0;

// epoll-based I/O readiness
static int gEpollFd = -1;
static size_t gArmedWaiters = 0; // fibers currently parked on an fd

// Fiber registry: a slot array with a free-list for reuse. Ids handed to
// Smalltalk pack a generation counter in the high bits so that a stale Process
// referring to a recycled slot cannot act on the fiber that replaced it.
// 24 bits of slot (16M concurrent fibers) + 38 bits of generation, fitting the
// 62-bit SmallInteger budget with room to spare.
#define ID_SLOT_BITS 24
#define ID_SLOT_MASK ((((size_t) 1) << ID_SLOT_BITS) - 1)
#define idSlot(id)   ((id) & ID_SLOT_MASK)

static Fiber **gFibers = NULL;
static size_t *gSlotGeneration = NULL; // per-slot generation, bumped on free
static size_t gFiberSlots = 0;
static size_t gFiberCap = 0;
static size_t *gFreeIds = NULL;
static size_t gFreeCount = 0;
static size_t gFreeCap = 0;

#define MAIN_STACK_SIZE   (8 * 1024 * 1024)
#define WORKER_STACK_SIZE (512 * 1024)


// ---- registry -------------------------------------------------------------

static size_t registerFiber(Fiber *fiber)
{
	size_t slot;
	if (gFreeCount > 0) {
		slot = gFreeIds[--gFreeCount];
	} else {
		if (gFiberSlots == gFiberCap) {
			gFiberCap = gFiberCap ? gFiberCap * 2 : 16;
			gFibers = realloc(gFibers, gFiberCap * sizeof(Fiber *));
			gSlotGeneration = realloc(gSlotGeneration, gFiberCap * sizeof(size_t));
		}
		slot = gFiberSlots++;
		gSlotGeneration[slot] = 0;
	}
	gFibers[slot] = fiber;
	fiber->id = (gSlotGeneration[slot] << ID_SLOT_BITS) | slot;
	return fiber->id;
}


static void unregisterFiber(Fiber *fiber)
{
	size_t slot = idSlot(fiber->id);
	gFibers[slot] = NULL;
	gSlotGeneration[slot]++;
	if (gFreeCount == gFreeCap) {
		gFreeCap = gFreeCap ? gFreeCap * 2 : 16;
		gFreeIds = realloc(gFreeIds, gFreeCap * sizeof(size_t));
	}
	gFreeIds[gFreeCount++] = slot;
}


// Resolve a Smalltalk-visible id back to its fiber, honouring the generation
// tag: returns NULL if the slot was recycled for a different fiber.
static Fiber *fiberFromId(size_t id)
{
	size_t slot = idSlot(id);
	if (slot >= gFiberSlots) {
		return NULL;
	}
	Fiber *fiber = gFibers[slot];
	return (fiber != NULL && fiber->id == id) ? fiber : NULL;
}


// ---- ready queue ----------------------------------------------------------

static void readyPush(Fiber *fiber)
{
	fiber->queueNext = NULL;
	if (gReadyTail == NULL) {
		gReadyHead = gReadyTail = fiber;
	} else {
		gReadyTail->queueNext = fiber;
		gReadyTail = fiber;
	}
}


static Fiber *readyPop(void)
{
	Fiber *fiber = gReadyHead;
	if (fiber != NULL) {
		gReadyHead = fiber->queueNext;
		if (gReadyHead == NULL) {
			gReadyTail = NULL;
		}
		fiber->queueNext = NULL;
	}
	return fiber;
}


static void readyRemove(Fiber *fiber)
{
	Fiber *prev = NULL;
	Fiber *node = gReadyHead;
	while (node != NULL) {
		if (node == fiber) {
			if (prev == NULL) {
				gReadyHead = node->queueNext;
			} else {
				prev->queueNext = node->queueNext;
			}
			if (gReadyTail == node) {
				gReadyTail = prev;
			}
			node->queueNext = NULL;
			return;
		}
		prev = node;
		node = node->queueNext;
	}
}


// ---- timer heap -----------------------------------------------------------

static void timerPush(int64_t deadline, size_t fiberId)
{
	if (gTimerCount == gTimerCap) {
		gTimerCap = gTimerCap ? gTimerCap * 2 : 16;
		gTimers = realloc(gTimers, gTimerCap * sizeof(Timer));
	}
	size_t i = gTimerCount++;
	gTimers[i].deadline = deadline;
	gTimers[i].fiberId = fiberId;
	while (i > 0) {
		size_t parent = (i - 1) / 2;
		if (gTimers[parent].deadline <= gTimers[i].deadline) {
			break;
		}
		Timer tmp = gTimers[parent];
		gTimers[parent] = gTimers[i];
		gTimers[i] = tmp;
		i = parent;
	}
}


static void timerPop(void)
{
	gTimers[0] = gTimers[--gTimerCount];
	size_t i = 0;
	for (;;) {
		size_t left = 2 * i + 1;
		size_t right = 2 * i + 2;
		size_t smallest = i;
		if (left < gTimerCount && gTimers[left].deadline < gTimers[smallest].deadline) {
			smallest = left;
		}
		if (right < gTimerCount && gTimers[right].deadline < gTimers[smallest].deadline) {
			smallest = right;
		}
		if (smallest == i) {
			break;
		}
		Timer tmp = gTimers[smallest];
		gTimers[smallest] = gTimers[i];
		gTimers[i] = tmp;
		i = smallest;
	}
}


// Wake every fiber whose deadline has passed; returns how many were woken.
static size_t wakeExpiredTimers(void)
{
	int64_t now = osCurrentMicroTime();
	size_t woken = 0;
	while (gTimerCount > 0 && gTimers[0].deadline <= now) {
		size_t id = gTimers[0].fiberId;
		timerPop();
		schedulerResume(id); // no-op if the fiber was terminated meanwhile
		woken++;
	}
	return woken;
}


// ---- root save / restore --------------------------------------------------

static void saveRoots(Fiber *fiber)
{
	fiber->roots.stackFramesTail = CurrentThread.stackFramesTail;
	fiber->roots.handleScopes = CurrentThread.handleScopes;
	fiber->roots.context = CurrentThread.context;
	fiber->roots.exceptionHandler = CurrentExceptionHandler;
}


static void loadRoots(Fiber *fiber)
{
	CurrentThread.stackFramesTail = fiber->roots.stackFramesTail;
	CurrentThread.handleScopes = fiber->roots.handleScopes;
	CurrentThread.context = fiber->roots.context;
	CurrentExceptionHandler = fiber->roots.exceptionHandler;
}


// Give each fiber its own root MethodContext so the entry trampoline and the
// lazy context-reification machinery have somewhere to hang off.
static void initFiberContext(Fiber *fiber)
{
	RawContext *context = (RawContext *) allocateObject(
		&CurrentThread.heap, Handles.MethodContext->raw, 0);
	context->thread = &CurrentThread;
	fiber->roots.stackFramesTail = NULL;
	fiber->roots.handleScopes = NULL;
	fiber->roots.context = tagPtr(context);
	fiber->roots.exceptionHandler = 0;
}


// ---- context switching ----------------------------------------------------

// From within a fiber: save its live roots and return control to the scheduler.
static void yieldToScheduler(void)
{
	Fiber *self = gCurrent;
	saveRoots(self);
	fiberSwitchAsm(&self->sp, gScheduler.sp);
	// Resumed later: the scheduler has already reloaded our roots and set
	// gCurrent back to us before switching in.
}


// From the scheduler: make `fiber` current and run it until it yields back.
static void runFiber(Fiber *fiber)
{
	gCurrent = fiber;
	fiber->state = FIBER_RUNNING;
	loadRoots(fiber);
	fiberSwitchAsm(&gScheduler.sp, fiber->sp);
	gCurrent = NULL;
}


// ---- fiber body -----------------------------------------------------------

static void runBlock(Fiber *fiber)
{
	HandleScope scope;
	openHandleScope(&scope);

	String *valueSelector = getSymbol("value");
	EntryArgs args = { .size = 0 };
	entryArgsAdd(&args, fiber->entryBlock); // read fresh (GC may have moved it)
	sendMessage(valueSelector, &args);

	closeHandleScope(&scope, NULL);
}


void schedulerFiberMain(void)
{
	Fiber *self = gCurrent;
	if (self->cEntry != NULL) {
		self->cEntry(self->cArg);
	} else {
		runBlock(self);
	}

	self->state = FIBER_DONE;
	// Return to the scheduler for good; it will destroy us. Never comes back.
	fiberSwitchAsm(&self->sp, gScheduler.sp);
	abort();
}


// ---- public API -----------------------------------------------------------

void schedulerInit(void)
{
	gScheduler.stackBase = NULL;
	gScheduler.sp = NULL;
	gCurrent = NULL;
	gEpollFd = epoll_create1(0);
	// Writing to a peer that has closed its end must return EPIPE, not kill us.
	signal(SIGPIPE, SIG_IGN);
	gActive = 1;
}


Fiber *schedulerSpawnC(FiberCEntry entry, void *arg, size_t stackSize)
{
	Fiber *fiber = fiberCreate(stackSize ? stackSize : MAIN_STACK_SIZE);
	fiber->cEntry = entry;
	fiber->cArg = arg;
	fiber->entryBlock = 0;
	initFiberContext(fiber);
	registerFiber(fiber);
	fiber->state = FIBER_READY;
	readyPush(fiber);
	return fiber;
}


size_t schedulerSpawnBlock(Value block)
{
	HandleScope scope;
	openHandleScope(&scope);
	Object *blockHandle = scopeHandle(asObject(block));

	Fiber *fiber = fiberCreate(WORKER_STACK_SIZE);
	fiber->cEntry = NULL;
	fiber->entryBlock = getTaggedPtr(blockHandle);
	registerFiber(fiber);          // entryBlock is a GC root from here on
	initFiberContext(fiber);       // may GC; entryBlock stays live
	fiber->state = FIBER_SUSPENDED;

	size_t id = fiber->id;
	closeHandleScope(&scope, NULL);
	return id;
}


void schedulerResume(size_t id)
{
	Fiber *fiber = fiberFromId(id);
	if (fiber != NULL && fiber->state == FIBER_SUSPENDED) {
		fiber->state = FIBER_READY;
		readyPush(fiber);
	}
}


void schedulerYield(void)
{
	Fiber *self = gCurrent;
	if (self == NULL) {
		return;
	}
	self->state = FIBER_READY;
	readyPush(self);
	yieldToScheduler();
}


void schedulerSuspend(void)
{
	Fiber *self = gCurrent;
	if (self == NULL) {
		return;
	}
	self->state = FIBER_SUSPENDED;
	yieldToScheduler(); // parked; resumes when schedulerResume(id) is called
}


void schedulerSleep(int64_t micros)
{
	Fiber *self = gCurrent;
	if (self == NULL) {
		return;
	}
	if (micros < 0) {
		micros = 0;
	}
	timerPush(osCurrentMicroTime() + micros, self->id);
	schedulerSuspend();
}


void schedulerWaitFd(int fd, int forWrite)
{
	Fiber *self = gCurrent;
	if (self == NULL || gEpollFd < 0) {
		return;
	}

	struct epoll_event ev;
	ev.events = (forWrite ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
	ev.data.u64 = self->id;
	// Re-arm if the fd is already known to epoll, otherwise register it.
	if (epoll_ctl(gEpollFd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT) {
		epoll_ctl(gEpollFd, EPOLL_CTL_ADD, fd, &ev);
	}

	gArmedWaiters++;
	schedulerSuspend(); // resumed by the event loop when the fd is ready
}


void schedulerTerminate(size_t id)
{
	if (!gActive) {
		// Before the scheduler is running (e.g. an unhandled exception during
		// bootstrap) terminating "the process" means exiting the VM.
		exit(1);
	}
	Fiber *fiber = fiberFromId(id);
	if (fiber == NULL) {
		return;
	}

	if (fiber == gCurrent) {
		fiber->state = FIBER_DONE;
		fiberSwitchAsm(&fiber->sp, gScheduler.sp); // never returns
		abort();
	}

	if (fiber->state == FIBER_READY) {
		readyRemove(fiber);
	}
	fiber->state = FIBER_DONE;
	unregisterFiber(fiber);
	fiberDestroy(fiber);
}


size_t schedulerCurrentId(void)
{
	return gCurrent != NULL ? gCurrent->id : 0;
}


// Nothing is runnable: block in epoll until an fd becomes ready or the next
// timer is due, then wake the corresponding fibers. This single call serves
// both socket readiness and Delay-style sleeps.
static void waitForEvents(void)
{
	int timeout = -1; // block indefinitely by default
	if (gTimerCount > 0) {
		int64_t wait = gTimers[0].deadline - osCurrentMicroTime();
		timeout = wait <= 0 ? 0 : (int) ((wait + 999) / 1000);
	}

	struct epoll_event events[64];
	int n = epoll_wait(gEpollFd, events, 64, timeout);
	for (int i = 0; i < n; i++) {
		if (gArmedWaiters > 0) {
			gArmedWaiters--;
		}
		schedulerResume((size_t) events[i].data.u64);
	}
	wakeExpiredTimers();
}


Value schedulerRun(void)
{
	for (;;) {
		Fiber *fiber = readyPop();
		if (fiber == NULL) {
			// Nothing runnable. If fibers are sleeping or waiting on I/O, block
			// until something happens; otherwise there is no more work to do.
			if (gArmedWaiters > 0 || gTimerCount > 0) {
				waitForEvents();
				continue;
			}
			break;
		}

		runFiber(fiber);

		if (fiber->state == FIBER_DONE) {
			unregisterFiber(fiber);
			fiberDestroy(fiber);
		}
	}
	return gExitResult;
}


// ---- GC integration -------------------------------------------------------

_Bool schedulerActive(void)
{
	return gActive;
}


size_t schedulerFiberSlots(void)
{
	return gFiberSlots;
}


Fiber *schedulerFiberAt(size_t slot)
{
	return slot < gFiberSlots ? gFibers[slot] : NULL;
}


void schedulerSyncCurrentRoots(void)
{
	if (gActive && gCurrent != NULL) {
		saveRoots(gCurrent);
	}
}


void schedulerRestoreCurrentRoots(void)
{
	if (gActive && gCurrent != NULL) {
		loadRoots(gCurrent);
	}
}
