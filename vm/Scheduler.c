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
#include <string.h>
#include <unistd.h>
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

// ---- what stays TLS (one copy per worker OS thread) -----------------------
// gScheduler is this worker's OWN OS-thread stack context (must not be shared);
// gCurrent is the fiber running on THIS worker; gActive/gExitResult track this
// worker's scheduler status. Everything else moved into the shared per-heap
// `struct Scheduler` below so a pool of workers shares ONE ready queue/registry.
static PER_ISOLATE Fiber gScheduler;          // scheduler context (stackBase == NULL)
static PER_ISOLATE Fiber *gCurrent = NULL;    // running fiber, NULL while in the scheduler
static PER_ISOLATE _Bool gActive = 0;
static PER_ISOLATE Value gExitResult = 0;

// timer min-heap entry: a fiber sleeping until a deadline (Delay wait)
typedef struct {
	int64_t deadline; // absolute microseconds
	size_t fiberId;
} Timer;

// Fiber registry id layout: ids handed to Smalltalk pack a generation counter in
// the high bits so a stale Process referring to a recycled slot cannot act on the
// fiber that replaced it. 24 bits of slot (16M concurrent fibers) + 38 bits of
// generation, fitting the 62-bit SmallInteger budget with room to spare.
#define ID_SLOT_BITS 24
#define ID_SLOT_MASK ((((size_t) 1) << ID_SLOT_BITS) - 1)
#define idSlot(id)   ((id) & ID_SLOT_MASK)

// ---- shared per-heap scheduler state (was PER_ISOLATE TLS) -----------------
// One instance per heap, reachable as heap->sched, allocated by schedulerInit.
// Migrating this out of TLS is what lets M worker OS threads share ONE ready
// queue, fiber registry, timer heap and epoll instance over one heap. (Isolates
// keep independent schedulers because they have independent heaps.)
struct Scheduler {
	// ready queue (intrusive FIFO through Fiber.queueNext)
	Fiber *readyHead;
	Fiber *readyTail;
	// timer min-heap: fibers sleeping until a deadline
	Timer *timers;
	size_t timerCount;
	size_t timerCap;
	// epoll-based I/O readiness
	int epollFd;
	size_t armedWaiters; // fibers currently parked on an fd
	// registry: a slot array with a free-list for reuse
	Fiber **fibers;
	size_t *slotGeneration; // per-slot generation, bumped on free
	size_t fiberSlots;
	size_t fiberCap;
	size_t *freeIds;
	size_t freeCount;
	size_t freeCap;
	// GC dirty-fiber list head (fibers that may hold a young root directly)
	Fiber *dirtyHead;
	// --- worker pool coordination (all under `lock`) ---
	size_t runningCount;   // fibers currently RUNNING or mid-commit on some worker
	int idleWorkers;       // workers parked on `work` waiting for something to run
	int pollerActive;      // 1 while some worker owns epoll_wait (exactly one at a time)
	int shutdown;          // set when all work drained; tells every worker to exit
	int workerCount;       // OS-thread workers running the run loop (ST_SCHED_WORKERS)
	size_t workerStackSize; // per-fiber stack reservation for schedulerSpawnBlock
	size_t initialCommit;   // initial committed stack window (for helper fiberInitStackGrowth)
	SmalltalkHandles handles; // well-known-symbol snapshot replicated into helper workers
	pthread_cond_t work;   // signalled when a fiber becomes ready / on shutdown
	// Guards ALL of the above so M worker OS threads can share this one scheduler.
	// A LEAF lock: no code path allocates or hits a GC safepoint while holding it, so
	// it never nests under heap->gcLock and cannot deadlock the STW handshake. Never
	// held across a fiber context switch (the park handoff releases it before the
	// switch and re-takes it in the run loop's commit).
	pthread_mutex_t lock;
};

// The current heap's shared scheduler. Only reached from scheduler code, which runs
// after schedulerInit allocated heap->sched. The `g*` names below stay so the rest
// of this file reads unchanged; each now resolves to a field of heap->sched.
static inline struct Scheduler *curSched(void) { return CurrentThread.heap->sched; }

#define gReadyHead      (curSched()->readyHead)
#define gReadyTail      (curSched()->readyTail)
#define gTimers         (curSched()->timers)
#define gTimerCount     (curSched()->timerCount)
#define gTimerCap       (curSched()->timerCap)
#define gEpollFd        (curSched()->epollFd)
#define gArmedWaiters   (curSched()->armedWaiters)
#define gFibers         (curSched()->fibers)
#define gSlotGeneration (curSched()->slotGeneration)
#define gFiberSlots     (curSched()->fiberSlots)
#define gFiberCap       (curSched()->fiberCap)
#define gFreeIds        (curSched()->freeIds)
#define gFreeCount      (curSched()->freeCount)
#define gFreeCap        (curSched()->freeCap)
#define gDirtyHead      (curSched()->dirtyHead)

static inline void schedLock(void)   { pthread_mutex_lock(&curSched()->lock); }
static inline void schedUnlock(void) { pthread_mutex_unlock(&curSched()->lock); }

// Make a fiber runnable; caller holds sched->lock. Forward-declared because the timer
// sweep (defined earlier) resumes through it.
static void schedulerResumeLocked(size_t id);
static void signalWork(void); // wake one idle worker; caller holds sched->lock

#define MAIN_STACK_SIZE   (8 * 1024 * 1024)
#define WORKER_STACK_SIZE (512 * 1024)

// Per-worker-fiber stack RESERVATION (PROT_NONE address space, tunable via
// ST_FIBER_STACK_KB, default 1 MB). Only a small window (ST_FIBER_COMMIT_KB,
// default 64 KB — see fiberInitStackGrowth) is committed RW; the SIGSEGV handler
// grows it downward on demand up to this reservation, then aborts at the floor.
static PER_ISOLATE size_t gWorkerStackSize = 1024 * 1024;


// ---- GC dirty-fiber list --------------------------------------------------
// Fibers that may hold a young pointer directly on their roots. The scavenger
// walks ONLY these (a fiber whose direct roots are all old is covered by the
// remembered set and skipped). A fiber becomes dirty when registered or run, and
// is cleaned by the scavenger once its walk finds no young direct root. Doubly
// linked so the scavenger can unlink a cleaned fiber in O(1). (gDirtyHead now lives
// in the shared per-heap struct Scheduler, above.)

static void schedulerMarkFiberDirty(Fiber *fiber)
{
	if (fiber->dirty) {
		return;
	}
	fiber->dirty = 1;
	fiber->dirtyPrev = NULL;
	fiber->dirtyNext = gDirtyHead;
	if (gDirtyHead != NULL) {
		gDirtyHead->dirtyPrev = fiber;
	}
	gDirtyHead = fiber;
}

void schedulerMarkFiberClean(Fiber *fiber)
{
	if (!fiber->dirty) {
		return;
	}
	fiber->dirty = 0;
	if (fiber->dirtyPrev != NULL) {
		fiber->dirtyPrev->dirtyNext = fiber->dirtyNext;
	} else {
		gDirtyHead = fiber->dirtyNext;
	}
	if (fiber->dirtyNext != NULL) {
		fiber->dirtyNext->dirtyPrev = fiber->dirtyPrev;
	}
	fiber->dirtyNext = fiber->dirtyPrev = NULL;
}

Fiber *schedulerDirtyHead(void)
{
	return gDirtyHead;
}

Fiber *schedulerCurrentFiber(void)
{
	return gCurrent;
}


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
	// Dirty from birth — before initFiberContext (which allocates and can
	// scavenge) could otherwise skip this fiber and drop its young entryBlock.
	schedulerMarkFiberDirty(fiber);
	return fiber->id;
}


static void unregisterFiber(Fiber *fiber)
{
	schedulerMarkFiberClean(fiber); // unlink before the struct is freed
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


// Wake every fiber whose deadline has passed; returns how many were woken. Caller
// holds sched->lock (called from waitForEvents), so it resumes via the locked leaf.
static size_t wakeExpiredTimers(void)
{
	int64_t now = osCurrentMicroTime();
	size_t woken = 0;
	while (gTimerCount > 0 && gTimers[0].deadline <= now) {
		size_t id = gTimers[0].fiberId;
		timerPop();
		schedulerResumeLocked(id); // no-op if the fiber was terminated meanwhile
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


// Re-point EVERY live reified context of `fiber` at the current worker's Thread. The JIT
// reads `CTX->thread` to reach per-mutator state (TLAB bump, remembered-set log,
// stackFramesTail, the dummy context), and each reified context caches `thread` (copied
// from its parent when it was created). With a worker pool a fiber can resume on a
// DIFFERENT worker than the one it parked on, so those cached pointers must be rebound or
// it allocates into / logs into another worker's per-mutator state and corrupts the heap.
// Every live context sits in some stack frame's CONTEXT_SLOT (plus the fiber's root
// context, shared by all non-reified methods), so walk the frames exactly like the GC.
static void fiberRebindContexts(Fiber *fiber)
{
	Thread *me = &CurrentThread;
	// Root context (used directly by every method compiled without hasContext).
	((RawContext *) asObject(fiber->roots.context))->thread = me;
	for (EntryStackFrame *entryFrame = fiber->roots.stackFramesTail;
	     entryFrame != NULL; entryFrame = entryFrame->prev) {
		for (StackFrame *frame = entryFrame->exit; frame != NULL;
		     frame = stackFrameGetParent(frame, entryFrame)) {
			Value ctx = stackFrameGetSlot(frame, CONTEXT_SLOT);
			if (valueTypeOf(ctx, VALUE_POINTER)) {
				((RawContext *) asObject(ctx))->thread = me;
			}
		}
	}
}


static void loadRoots(Fiber *fiber)
{
	CurrentThread.stackFramesTail = fiber->roots.stackFramesTail;
	CurrentThread.handleScopes = fiber->roots.handleScopes;
	CurrentThread.context = fiber->roots.context;
	CurrentExceptionHandler = fiber->roots.exceptionHandler;
	// Rebind this fiber's contexts to the worker about to run it — but ONLY when it
	// actually migrated. If its root context already points at us, this fiber last ran on
	// this worker, so every context it holds already points here (nothing else rewrites
	// them). That makes the common same-worker resume O(1) and pays the frame walk only on
	// a real migration.
	if (fiber->roots.context != 0
	    && ((RawContext *) asObject(fiber->roots.context))->thread != &CurrentThread) {
		fiberRebindContexts(fiber);
	}
}


// Give each fiber its own root MethodContext so the entry trampoline and the
// lazy context-reification machinery have somewhere to hang off.
static void initFiberContext(Fiber *fiber)
{
	RawContext *context = (RawContext *) allocateObject(
		CurrentThread.heap, Handles.MethodContext->raw, 0);
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
	TSAN_SWITCH_TO_FIBER(gScheduler.tsanFiber); // tell TSan we're back on the scheduler stack
	fiberSwitchAsm(&self->sp, gScheduler.sp);
	// Resumed later: the scheduler has already reloaded our roots and set
	// gCurrent back to us before switching in.
}


// From the scheduler: make `fiber` current and run it until it yields back.
static void runFiber(Fiber *fiber)
{
	gCurrent = fiber;
	fiber->state = FIBER_RUNNING;
	schedulerMarkFiberDirty(fiber); // running code may create new young roots
	loadRoots(fiber);
	TSAN_SWITCH_TO_FIBER(fiber->tsanFiber); // tell TSan we're switching onto this fiber's stack
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
	TSAN_SWITCH_TO_FIBER(gScheduler.tsanFiber);
	fiberSwitchAsm(&self->sp, gScheduler.sp);
	abort();
}


// ---- public API -----------------------------------------------------------

// ---- growable-stack SIGSEGV handler --------------------------------------
// A downward stack overflow into a fiber's reserved-but-uncommitted window
// faults; this handler (running on a per-thread sigaltstack, since the fault
// happens with RSP at the guard) grows the committed window and retries. A fault
// outside the current fiber's window / below the floor is a genuine bug → restore
// the default handler and re-raise for a core dump at the real PC.

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
static PER_ISOLATE char *gAltStack = NULL;   // per-thread signal stack
static int gSegvHandlerInstalled = 0;        // process-global, once

static void fiberSegvHandler(int sig, siginfo_t *si, void *ucontext)
{
	(void) sig;
	(void) ucontext;
	Fiber *fiber = gCurrent; // initial-exec TLS load: async-signal-safe
	if (fiber != NULL
#ifdef SEGV_ACCERR
	    && si->si_code == SEGV_ACCERR // mapped-but-PROT_NONE, i.e. our reserved window
#endif
	    && fiberGrowStack(fiber, (uintptr_t) si->si_addr)) {
		return; // grown -> retry the faulting instruction
	}
	static const char msg[] = "fatal: stack overflow past reservation / invalid memory access\n";
	ssize_t w = write(2, msg, sizeof(msg) - 1);
	(void) w;
	signal(SIGSEGV, SIG_DFL); // return re-faults into the default handler -> core at real PC
}

static void installStackGrowthHandler(void)
{
	if (gAltStack == NULL) {
		size_t altSize = 32 * 1024; // handler is tiny; SIGSTKSZ headroom
		gAltStack = malloc(altSize);
		if (gAltStack != NULL) {
			stack_t ss;
			ss.ss_sp = gAltStack;
			ss.ss_size = altSize;
			ss.ss_flags = 0;
			sigaltstack(&ss, NULL); // per-thread
		}
	}
	if (__sync_bool_compare_and_swap(&gSegvHandlerInstalled, 0, 1)) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = fiberSegvHandler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // NOT SA_NODEFER / SA_RESETHAND
		sigaction(SIGSEGV, &sa, NULL);         // process-global, once
	}
}
#else
static void installStackGrowthHandler(void) {} // ASan owns SIGSEGV + intercepts mmap/mprotect
#endif


void schedulerInit(void)
{
	Heap *heap = CurrentThread.heap;

	// Fiber stack sizing (per-thread caches; env read once, values idempotent).
	char *stackEnv = getenv("ST_FIBER_STACK_KB"); // reservation ceiling
	if (stackEnv != NULL) {
		long kb = atol(stackEnv);
		if (kb >= 64) {
			gWorkerStackSize = (size_t) kb * 1024;
		}
	}
	size_t commit = 64 * 1024;
	char *commitEnv = getenv("ST_FIBER_COMMIT_KB");
	if (commitEnv != NULL) {
		long kb = atol(commitEnv);
		if (kb >= 16) {
			commit = (size_t) kb * 1024;
		}
	}

	if (heap->sched == NULL) {
		// First scheduler activation on this heap: allocate the SHARED state (ready
		// queue, registry, timers, epoll, pool coordination). Later workers of the
		// same heap reuse it.
		struct Scheduler *sched = calloc(1, sizeof(struct Scheduler));
		sched->epollFd = epoll_create1(0);
		pthread_mutex_init(&sched->lock, NULL);
		pthread_cond_init(&sched->work, NULL);
		// Worker-pool size: ST_SCHED_WORKERS OS threads share this scheduler. Default 1
		// (byte-for-byte today's single-threaded scheduler) until the Smalltalk sync
		// primitives are made thread-safe; then the default flips to the core count.
		sched->workerCount = 1;
		char *workersEnv = getenv("ST_SCHED_WORKERS");
		if (workersEnv != NULL) {
			long n = atol(workersEnv);
			if (n >= 1 && n <= 1024) {
				sched->workerCount = (int) n;
			}
		}
		sched->workerStackSize = gWorkerStackSize;
		sched->initialCommit = commit;
		sched->handles = Handles; // snapshot of well-known symbols for helper workers
		heap->sched = sched;
	}

	// Per-OS-thread (TLS) setup — runs for every worker that joins this scheduler.
	gScheduler.stackBase = NULL;
	gScheduler.sp = NULL;
	gScheduler.tsanFiber = TSAN_CURRENT_FIBER(); // this worker's native-stack fiber (for TSan)
	gCurrent = NULL;
	// Writing to a peer that has closed its end must return EPIPE, not kill us.
	signal(SIGPIPE, SIG_IGN);
	gActive = 1;

	// Publish the SHARED fiber registry + this worker's current-fiber/handler slots
	// so a GC collector on ANOTHER OS thread can reach this mutator's live roots (the
	// Fiber structs are heap-allocated; schedCurrent points at THIS worker's TLS).
	CurrentThread.schedFibers = &gFibers;
	CurrentThread.schedFiberSlots = &gFiberSlots;
	CurrentThread.schedCurrent = &gCurrent;
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler; // this thread's own TLS slot

	fiberInitStackGrowth(commit); // caches page size + initial commit for fiberCreate

	installStackGrowthHandler();  // per-thread sigaltstack + once-global SIGSEGV handler
}


Fiber *schedulerSpawnC(FiberCEntry entry, void *arg, size_t stackSize)
{
	Fiber *fiber = fiberCreate(stackSize ? stackSize : MAIN_STACK_SIZE);
	fiber->cEntry = entry;
	fiber->cArg = arg;
	fiber->entryBlock = 0;
	initFiberContext(fiber); // allocates (no lock held); no scavenge window drops a root
	schedLock();
	registerFiber(fiber);
	fiber->state = FIBER_READY;
	readyPush(fiber);
	signalWork();
	schedUnlock();
	return fiber;
}


size_t schedulerSpawnBlock(Value block)
{
	HandleScope scope;
	openHandleScope(&scope);
	Object *blockHandle = scopeHandle(asObject(block));

	Fiber *fiber = fiberCreate(gWorkerStackSize);
	fiber->cEntry = NULL;
	fiber->entryBlock = getTaggedPtr(blockHandle);
	schedLock();
	registerFiber(fiber);          // entryBlock is a GC root from here on (fiber is dirty)
	schedUnlock();
	initFiberContext(fiber);       // may GC; entryBlock stays live (fiber is registered)
	fiber->state = FIBER_SUSPENDED; // left off the ready queue; no id published yet, so no racing resume

	size_t id = fiber->id;
	closeHandleScope(&scope, NULL);
	return id;
}


// Wake one worker parked on the ready condvar (a fiber just became runnable). Caller
// holds sched->lock. No-op when single-worker (no one is ever parked on `work`).
static void signalWork(void)
{
	pthread_cond_signal(&curSched()->work);
}


// Make a fiber runnable. Caller holds sched->lock. This is the leaf used by the event
// loop, by timers, and by the public schedulerResume.
static void schedulerResumeLocked(size_t id)
{
	Fiber *fiber = fiberFromId(id);
	if (fiber == NULL) {
		return;
	}
	if (fiber->state == FIBER_SUSPENDED) {
		// Fully parked (its stack is off any worker): make it runnable now.
		fiber->state = FIBER_READY;
		readyPush(fiber);
		signalWork();
	} else if (fiber->state == FIBER_PARKING) {
		// It chose to suspend but has not finished switching off its worker's stack.
		// Record the wake; the run loop's commit will re-queue it instead of parking,
		// so this resume is never lost (and we never make it runnable mid-switch).
		fiber->parkPending = 1;
	}
	// READY / RUNNING / DONE: no-op (matches the old SUSPENDED-only guard).
}


void schedulerResume(size_t id)
{
	schedLock();
	schedulerResumeLocked(id);
	schedUnlock();
}


// Record the current fiber's park intent and switch off to this worker's scheduler
// stack. Caller holds sched->lock; parkSelf RELEASES it before the switch (never hold
// the scheduler lock across a context switch). The fiber does NOT re-queue itself: the
// run loop re-queues/parks it AFTER the switch completes, so no other worker can pop
// and run this same stack while it is still switching (no "same stack on two threads").
static void parkSelf(ParkIntent intent)
{
	Fiber *self = gCurrent;
	self->parkIntent = intent;
	self->parkPending = 0;
	self->state = FIBER_PARKING;
	schedUnlock();
	yieldToScheduler(); // committed by the run loop after the switch
}


void schedulerYield(void)
{
	if (gCurrent == NULL) {
		return;
	}
	schedLock();
	parkSelf(PARK_YIELD); // releases the lock, switches; committed to READY by the run loop
}


void schedulerSuspend(void)
{
	if (gCurrent == NULL) {
		return;
	}
	schedLock();
	parkSelf(PARK_SUSPEND); // committed to SUSPENDED (or re-queued if a resume raced)
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
	// Register the timer AND transition to PARKING under one lock hold, so a poller on
	// another worker cannot fire (and consume) the timer between the push and the park
	// — it either runs before the push (timer not yet there) or after PARKING is set (a
	// resume then sets parkPending, never lost).
	schedLock();
	timerPush(osCurrentMicroTime() + micros, self->id);
	parkSelf(PARK_SUSPEND); // releases the lock, switches
}


void schedulerWaitFd(int fd, int forWrite)
{
	Fiber *self = gCurrent;
	if (self == NULL || gEpollFd < 0) {
		return;
	}

	// Arm epoll AND transition to PARKING under one lock hold (same lost-wakeup reason
	// as schedulerSleep: an already-ready fd could fire immediately on the poller).
	schedLock();
	struct epoll_event ev;
	ev.events = (forWrite ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
	ev.data.u64 = self->id;
	// Re-arm if the fd is already known to epoll, otherwise register it.
	if (epoll_ctl(gEpollFd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT) {
		epoll_ctl(gEpollFd, EPOLL_CTL_ADD, fd, &ev);
	}
	gArmedWaiters++;
	self->waitFd = fd;  // recorded so schedulerTerminate can disarm a killed waiter
	parkSelf(PARK_SUSPEND); // resumed by the event loop when the fd is ready
	self->waitFd = -1;  // woken normally: the event loop already accounted for us
}


void schedulerTerminate(size_t id)
{
	if (!gActive) {
		// Before the scheduler is running (e.g. an unhandled exception during
		// bootstrap) terminating "the process" means exiting the VM.
		exit(1);
	}
	schedLock();
	Fiber *fiber = fiberFromId(id);
	if (fiber == NULL) {
		schedUnlock();
		return;
	}

	if (fiber == gCurrent) {
		fiber->state = FIBER_DONE;
		schedUnlock();
		TSAN_SWITCH_TO_FIBER(gScheduler.tsanFiber);
		fiberSwitchAsm(&fiber->sp, gScheduler.sp); // never returns; run loop commits DONE
		abort();
	}

	if (fiber->state == FIBER_READY) {
		readyRemove(fiber);
	} else if (fiber->state == FIBER_SUSPENDED && fiber->waitFd >= 0) {
		// Killing a fiber parked on an fd: undo its epoll arming and its armed-
		// waiter accounting, else the run loop keeps blocking on an event that
		// will never be dispatched to a live fiber (the isolate would hang).
		epoll_ctl(gEpollFd, EPOLL_CTL_DEL, fiber->waitFd, NULL);
		if (gArmedWaiters > 0) {
			gArmedWaiters--;
		}
		fiber->waitFd = -1;
	} else if (fiber->state == FIBER_RUNNING || fiber->state == FIBER_PARKING) {
		// Running or mid-park on ANOTHER worker: we cannot synchronously reclaim its
		// stack from here. (Unreachable at the default 1 worker, where a non-current
		// fiber is never RUNNING/PARKING; cross-worker terminate is handled in a later
		// increment.) Leave it be rather than corrupt a live stack.
		schedUnlock();
		return;
	}
	fiber->state = FIBER_DONE;
	unregisterFiber(fiber);
	schedUnlock();
	fiberDestroy(fiber); // munmap outside the lock; the fiber is already unregistered
}


size_t schedulerCurrentId(void)
{
	return gCurrent != NULL ? gCurrent->id : 0;
}


// The sole poller blocks in epoll until an fd becomes ready or the next timer is due,
// then wakes the corresponding fibers. Called WITHOUT sched->lock; clears pollerActive
// and wakes idle workers before returning. Serves both socket readiness and Delay sleeps.
static void waitForEvents(void)
{
	Heap *heap = CurrentThread.heap;
	struct Scheduler *s = curSched();

	schedLock();
	int timeout = -1; // block indefinitely by default
	if (gTimerCount > 0) {
		int64_t wait = gTimers[0].deadline - osCurrentMicroTime();
		timeout = wait <= 0 ? 0 : (int) ((wait + 999) / 1000);
	}
	schedUnlock();

	struct epoll_event events[64];
	// epoll_wait blocks: count as safe so a peer's stop-the-world GC isn't held up, then
	// leave the blocked state (waiting out any in-progress STW) before mutating the
	// scheduler as an active mutator.
	heapGcEnterBlocked(heap, &CurrentThread);
	int n = epoll_wait(gEpollFd, events, 64, timeout);
	heapGcLeaveBlocked(heap, &CurrentThread);

	schedLock();
	for (int i = 0; i < n; i++) {
		if (gArmedWaiters > 0) {
			gArmedWaiters--;
		}
		schedulerResumeLocked((size_t) events[i].data.u64);
	}
	wakeExpiredTimers(); // resumes via schedulerResumeLocked (lock held)
	s->pollerActive = 0;
	pthread_cond_broadcast(&s->work); // wake idle workers for the freshly-ready fibers
	schedUnlock();
}


// Post-run commit, run under sched->lock. runFiber has fully switched the fiber off
// THIS worker's stack (gCurrent == NULL again), so it is now safe to make it runnable
// (a peer worker could pop it), park it, or destroy it — the "same stack on two OS
// threads" hazard is closed because ready-queue membership is granted only here,
// strictly after the switch completed. May briefly drop the lock for a munmap.
static void commitFiber(Fiber *fiber)
{
	if (fiber->state == FIBER_DONE) {
		unregisterFiber(fiber);
		schedUnlock();
		fiberDestroy(fiber); // munmap outside the lock; the fiber is unregistered/unreachable
		schedLock();
		return;
	}
	// Otherwise it is FIBER_PARKING (it chose to yield or suspend).
	if (fiber->parkIntent == PARK_YIELD) {
		fiber->state = FIBER_READY;
		readyPush(fiber);
		signalWork();
	} else if (fiber->parkPending) {
		// A resume raced in while the fiber was still FIBER_PARKING: re-queue it instead
		// of parking, so the wakeup is not lost.
		fiber->parkPending = 0;
		fiber->state = FIBER_READY;
		readyPush(fiber);
		signalWork();
	} else {
		fiber->state = FIBER_SUSPENDED;
		// Hand the dead pages below its stack pointer back to the OS. Done UNDER the lock
		// (madvise is fast and non-blocking) so the fiber cannot be resumed and run into a
		// just-madvised stack: a racing resume must take the lock we still hold.
		fiberReleaseIdleStack(fiber);
	}
	fiber->parkIntent = PARK_NONE;
}


// One pool worker's run loop: pull a ready fiber and run it, commit its disposition,
// and when the queue is empty either drive the event loop (sole poller), idle-wait
// GC-safely, or shut the pool down when no work remains anywhere.
static void schedulerRunWorker(void)
{
	Heap *heap = CurrentThread.heap;
	struct Scheduler *s = curSched();
	for (;;) {
		schedLock();
		Fiber *fiber = readyPop();
		if (fiber != NULL) {
			s->runningCount++;
			schedUnlock();

			runFiber(fiber);

			schedLock();
			s->runningCount--;
			commitFiber(fiber);
			schedUnlock();
			continue;
		}
		// Ready queue empty.
		if (s->shutdown) {
			schedUnlock();
			break;
		}
		if (s->runningCount == 0 && gArmedWaiters == 0 && gTimerCount == 0) {
			// No runnable fibers and nothing pending anywhere: shut the whole pool down.
			s->shutdown = 1;
			pthread_cond_broadcast(&s->work);
			schedUnlock();
			break;
		}
		if ((gArmedWaiters > 0 || gTimerCount > 0) && !s->pollerActive) {
			// Become the sole poller.
			s->pollerActive = 1;
			schedUnlock();
			waitForEvents(); // enter/leaveBlocked around epoll_wait; clears pollerActive
			continue;
		}
		// A peer is running work that may re-enqueue, or another worker polls: idle-wait,
		// GC-safe. heapGcEnterBlocked (spBlocked=1) makes a peer's STW not wait on us; the
		// cond_wait releases sched->lock while parked.
		s->idleWorkers++;
		schedUnlock();
		heapGcEnterBlocked(heap, &CurrentThread);
		schedLock();
		while (gReadyHead == NULL && !s->shutdown
		       && !((gArmedWaiters > 0 || gTimerCount > 0) && !s->pollerActive)) {
			pthread_cond_wait(&s->work, &s->lock);
		}
		s->idleWorkers--;
		schedUnlock();
		heapGcLeaveBlocked(heap, &CurrentThread); // wait out any in-progress STW before running
	}
}


// A spawned helper worker OS thread: set up its TLS to share the caller's heap and
// scheduler (mirrors parallelPrimWorker), then run the shared run loop.
static void *schedulerHelperMain(void *arg)
{
	Heap *heap = (Heap *) arg;
	struct Scheduler *s = heap->sched;

	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = heap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(heap, &CurrentThread);                      // register before any allocation
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler;
	Handles = s->handles;                                      // well-known symbols snapshot
	gWorkerStackSize = s->workerStackSize;
	initThreadContext(&CurrentThread);                         // allocates this worker's root context

	// Publish this worker's scheduler TLS (heap->sched already exists — do NOT re-run the
	// shared init). schedFibers/schedCurrent point at the SHARED registry / this worker's
	// TLS current fiber so a cross-thread collector can reach this worker's roots.
	gScheduler.stackBase = NULL;
	gScheduler.sp = NULL;
	gScheduler.tsanFiber = TSAN_CURRENT_FIBER(); // this worker's native-stack fiber (for TSan)
	gCurrent = NULL;
	gActive = 1;
	CurrentThread.schedFibers = &gFibers;
	CurrentThread.schedFiberSlots = &gFiberSlots;
	CurrentThread.schedCurrent = &gCurrent;
	// Do NOT call fiberInitStackGrowth here: it writes process-global page-size/commit
	// caches that the primary already set in schedulerInit BEFORE pthread_create (a
	// happens-before edge), so helpers only READ them. Re-writing them races (TSan). We
	// still need this worker's OWN sigaltstack for the growable-stack SIGSEGV handler.
	installStackGrowthHandler();

	schedulerRunWorker();

	heapEndMutator(heap, &CurrentThread); // deregister so a later GC never scans this dead thread
	return NULL;
}


Value schedulerRun(void)
{
	struct Scheduler *s = curSched();
	Heap *heap = CurrentThread.heap;
	int helpers = s->workerCount - 1;

	pthread_t *tids = NULL;
	if (helpers > 0) {
		tids = malloc((size_t) helpers * sizeof(pthread_t));
		for (int i = 0; i < helpers; i++) {
			pthread_create(&tids[i], NULL, schedulerHelperMain, heap);
		}
	}

	schedulerRunWorker(); // the calling thread participates as a worker

	if (helpers > 0) {
		// Our run loop returned (shutdown broadcast); wait for the helpers. Count as
		// safe while joining so a helper's final GC isn't blocked by us.
		heapGcEnterBlocked(heap, &CurrentThread);
		for (int i = 0; i < helpers; i++) {
			pthread_join(tids[i], NULL);
		}
		heapGcLeaveBlocked(heap, &CurrentThread);
		free(tids);
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


size_t schedulerLiveFibers(void)
{
	return gFiberSlots - gFreeCount;
}


size_t schedulerArmedWaiters(void)
{
	return gArmedWaiters;
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
