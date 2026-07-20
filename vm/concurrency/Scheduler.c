#define _GNU_SOURCE
#include "concurrency/Scheduler.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "os/OsThread.h"
#include "core/Handle.h"
#include "core/Entry.h"
#include "core/Smalltalk.h"
#include "core/Exception.h"
#include "core/StackFrame.h"
#include "os/Os.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include "os/Os.h"
#include <sched.h>

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

// Process-wide (NOT per-worker TLS: any worker's fiber can die) count of
// unhandled errors: Exception>>defaultAction bumps it just before terminating
// the signaling fiber. main() folds a nonzero count into the process exit code
// in non-interactive mode, so a test can no longer print a backtrace and still
// exit 0 (the historical false-pass). Atomic: fibers on different workers can
// fail simultaneously.
static int gUnhandledErrors = 0;

void schedulerNoteUnhandledError(void)
{
	__atomic_add_fetch(&gUnhandledErrors, 1, __ATOMIC_RELAXED);
}

int schedulerUnhandledErrors(void)
{
	return __atomic_load_n(&gUnhandledErrors, __ATOMIC_RELAXED);
}

// Read-and-clear, for tests that deliberately drive an error into
// defaultAction (e.g. the unhandled-in-fiber probe in UnwindTest) and must
// then both ASSERT it fired and keep the run's exit code clean.
int schedulerTakeUnhandledErrors(void)
{
	return __atomic_exchange_n(&gUnhandledErrors, 0, __ATOMIC_RELAXED);
}

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
// Migrating this out of TLS is what lets M worker OS threads share ONE fiber
// registry, timer heap and epoll instance over one heap. (Isolates keep
// independent schedulers because they have independent heaps.)
struct Scheduler {
	// Per-worker ready queues (intrusive FIFO through Fiber.queueNext). Fibers are
	// PINNED to a home worker (Fiber.homeWorker): a fiber only ever runs on, and is
	// re-queued to, its home worker's queue — it NEVER migrates OS threads. This
	// deliberately trades dynamic load-balancing (a deferred perf item: work-stealing)
	// for eliminating a fiber-migration corruption bug and keeping each fiber's C
	// stack + roots on a single OS thread its whole life. Real parallelism is kept:
	// distinct fibers run on distinct workers (round-robin home assignment at spawn).
	// Both arrays are workerCount long, allocated by schedulerInit.
	Fiber **readyHead;
	Fiber **readyTail;
	int nextHome; // round-robin cursor for assigning a new fiber's home worker
	// timer min-heap: fibers sleeping until a deadline
	Timer *timers;
	size_t timerCount;
	size_t timerCap;
	// I/O readiness multiplexer + poller wakeup (see vm/os/Os.h)
	OsEventLoop *events;
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
	OsCond work;           // signalled when a fiber becomes ready / on shutdown
	// Guards ALL of the above so M worker OS threads can share this one scheduler.
	// A LEAF lock: no code path allocates or hits a GC safepoint while holding it, so
	// it never nests under heap->gcLock and cannot deadlock the STW handshake. Never
	// held across a fiber context switch (the park handoff releases it before the
	// switch and re-takes it in the run loop's commit).
	OsMutex lock;
};

// The current heap's shared scheduler. Only reached from scheduler code, which runs
// after schedulerInit allocated heap->sched. The `g*` names below stay so the rest
// of this file reads unchanged; each now resolves to a field of heap->sched.
static inline struct Scheduler *curSched(void) { return CurrentThread.heap->sched; }

// This worker's index (0 = the schedulerRun thread, 1..N-1 = helpers). Selects which
// per-worker ready queue this thread pops from. A fiber's home worker selects which
// queue it is pushed to (Fiber.homeWorker). Both indices are < sched->workerCount.
static PER_ISOLATE int gWorkerIndex = 0;
#define gTimers         (curSched()->timers)
#define gTimerCount     (curSched()->timerCount)
#define gTimerCap       (curSched()->timerCap)
#define gEvents         (curSched()->events)
#define gArmedWaiters   (curSched()->armedWaiters)
#define gFibers         (curSched()->fibers)
#define gSlotGeneration (curSched()->slotGeneration)
#define gFiberSlots     (curSched()->fiberSlots)
#define gFiberCap       (curSched()->fiberCap)
#define gFreeIds        (curSched()->freeIds)
#define gFreeCount      (curSched()->freeCount)
#define gFreeCap        (curSched()->freeCap)
#define gDirtyHead      (curSched()->dirtyHead)

static inline void schedLock(void)   { osMutexLock(&curSched()->lock); }
static inline void schedUnlock(void) { osMutexUnlock(&curSched()->lock); }

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
	// Pin to a home worker, round-robin, so fibers spread across all workers for real
	// parallelism while each stays on ONE OS thread for life (no migration). Under
	// sched->lock (registerFiber's caller holds it), so nextHome needs no atomic.
	struct Scheduler *s = curSched();
	fiber->homeWorker = s->nextHome;
	s->nextHome = (s->nextHome + 1) % s->workerCount;
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
	// Enqueue on the fiber's HOME worker's queue (pinning): it will only ever be popped
	// and run by that one worker, never migrating OS threads.
	int w = fiber->homeWorker;
	struct Scheduler *s = curSched();
	fiber->queueNext = NULL;
	if (s->readyTail[w] == NULL) {
		s->readyHead[w] = s->readyTail[w] = fiber;
	} else {
		s->readyTail[w]->queueNext = fiber;
		s->readyTail[w] = fiber;
	}
}


// Pop the next ready fiber from THIS worker's own queue (gWorkerIndex).
static Fiber *readyPop(void)
{
	struct Scheduler *s = curSched();
	int w = gWorkerIndex;
	Fiber *fiber = s->readyHead[w];
	if (fiber != NULL) {
		s->readyHead[w] = fiber->queueNext;
		if (s->readyHead[w] == NULL) {
			s->readyTail[w] = NULL;
		}
		fiber->queueNext = NULL;
	}
	return fiber;
}


static void readyRemove(Fiber *fiber)
{
	struct Scheduler *s = curSched();
	int w = fiber->homeWorker;
	Fiber *prev = NULL;
	Fiber *node = s->readyHead[w];
	while (node != NULL) {
		if (node == fiber) {
			if (prev == NULL) {
				s->readyHead[w] = node->queueNext;
			} else {
				prev->queueNext = node->queueNext;
			}
			if (s->readyTail[w] == node) {
				s->readyTail[w] = prev;
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
	fiber->roots.unwindHandler = CurrentThread.unwindHandler;
}


static void loadRoots(Fiber *fiber)
{
	CurrentThread.stackFramesTail = fiber->roots.stackFramesTail;
	CurrentThread.handleScopes = fiber->roots.handleScopes;
	CurrentThread.context = fiber->roots.context;
	CurrentExceptionHandler = fiber->roots.exceptionHandler;
	CurrentThread.unwindHandler = fiber->roots.unwindHandler;
	// No context->thread rebind needed: JIT-generated code reaches per-mutator state
	// (TLAB, remembered set, stackFramesTail, the dummy context, on:do: chain) via the
	// running worker's TLS (%fs, see asmLoadTls), so a fiber that migrates OS threads
	// automatically uses whichever worker is executing it now.
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
	fiber->roots.unwindHandler = 0;
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

	// GC-safe: hold the block in a scope handle (which the GC updates), NOT a raw Value in
	// EntryArgs. sendMessage allocates (entry frame / context, and any scavenge triggered by
	// a peer at a safepoint) before consuming args[0]; a raw copy would go stale when the
	// block moves, sending #value to a moved/reclaimed object. Under many workers a peer
	// scavenge lands in that window on essentially every fiber, so this must be a handle.
	Object *block = scopeHandle(asObject(fiber->entryBlock));
	String *valueSelector = getSymbol("value");
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, block);
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

// Growth callback for the OS-installed SIGSEGV handler: async-signal-safe
// (an initial-exec TLS load + mprotect inside fiberGrowStack). Returns 1 when
// the fault was a guard fault on the current fiber's reserved window and the
// committed span grew — the faulting instruction is then retried.
static _Bool fiberSegvGrowCallback(uintptr_t faultAddr)
{
	Fiber *fiber = gCurrent;
	if (fiber == NULL) {
		return 0;
	}
	// qemu-user (the ppc64 bring-up vehicle) delivers si_addr = NULL for
	// guard faults, so the fault cannot be attributed to an address. Fall
	// back to growing the RUNNING fiber's window one chunk — by far the
	// likeliest fault source. A non-stack fault then re-faults after each
	// growth until the reserve floor is hit and the fatal path still fires
	// (bounded). Real kernels always fill si_addr and never take this branch.
	if (faultAddr == 0) {
		uintptr_t clow = (uintptr_t) fiber->committedLow;
		uintptr_t floor = (uintptr_t) fiber->reserveFloor;
		if (clow <= floor) {
			return 0; // fully grown already: a genuine wild access
		}
		return fiberGrowStack(fiber, clow - 1);
	}
	return fiberGrowStack(fiber, faultAddr);
}




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
		sched->events = osEventLoopCreate();
		osMutexInit(&sched->lock);
		osCondInit(&sched->work);
		// Worker-pool size: N OS threads share this scheduler, each running fibers pinned
		// to it. DEFAULT = one worker per available core (respects taskset/cgroup
		// affinity). The whole gate is green at N workers: shared-heap GC via STW
		// safepoints incl. the strengthened stack-root scan, per-heap sync monitor +
		// symbol lock, become: under STW, thread-safe actors, no fiber migration, the
		// per-worker TLS lookup cache (a baked &LookupCache immediate in shared send
		// code tore under the owner's concurrent rewrites -> wrong-method dispatch),
		// and the remembered-set barrier on old-space-born objects. `ST_SCHED_WORKERS`
		// overrides: a fixed count N>=1 (1 = the old cooperative single thread), or 0
		// for one-per-core explicitly.
		sched->workerCount = osAvailableCoreCount();
		char *workersEnv = getenv("ST_SCHED_WORKERS");
		if (workersEnv != NULL) {
			long n = atol(workersEnv);
			if (n == 0) {
				sched->workerCount = osAvailableCoreCount();
			} else if (n >= 1 && n <= 1024) {
				sched->workerCount = (int) n;
			}
		}
		// Per-worker ready queues, sized exactly to the worker count (fibers are pinned
		// to a home worker; see struct Scheduler). Zeroed = all queues empty.
		sched->readyHead = calloc((size_t) sched->workerCount, sizeof(Fiber *));
		sched->readyTail = calloc((size_t) sched->workerCount, sizeof(Fiber *));
		sched->nextHome = 0;
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
	osIgnoreBrokenPipe();
	gActive = 1;

	// Publish the SHARED fiber registry + this worker's current-fiber/handler slots
	// so a GC collector on ANOTHER OS thread can reach this mutator's live roots (the
	// Fiber structs are heap-allocated; schedCurrent points at THIS worker's TLS).
	CurrentThread.schedFibers = &gFibers;
	CurrentThread.schedFiberSlots = &gFiberSlots;
	CurrentThread.schedCurrent = &gCurrent;
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler; // this thread's own TLS slot
	CurrentThread.schedUnwindHandler = &CurrentThread.unwindHandler;

	fiberInitStackGrowth(commit); // caches page size + initial commit for fiberCreate

	osInstallSegvHandler(fiberSegvGrowCallback); // per-thread altstack + once-global handler
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
// Kick the sole poller out of epoll_wait by writing to the wake-eventfd. Called when a
// fiber becomes ready and a poller is active, so a fiber made ready on the POLLER's own
// per-worker queue is not stranded behind a blocking epoll_wait. Caller holds sched->lock.
static void wakePoller(struct Scheduler *s)
{
	if (s->pollerActive && s->events != NULL) {
		osEventLoopWake(s->events);
	}
}

static void signalWork(void)
{
	// Broadcast (not signal): fibers are pinned to per-worker queues sharing ONE condvar,
	// so we must wake the fiber's HOME worker, which a single signal might miss (waking an
	// idle peer whose own queue is empty → it re-sleeps and the home worker stays parked →
	// lost wakeup / hang). All woken workers re-check their own queue; non-home ones re-wait.
	struct Scheduler *s = curSched();
	osCondBroadcast(&s->work);
	wakePoller(s); // the target worker might be the one currently blocked in epoll_wait
}


// True iff every per-worker ready queue is empty. Caller holds sched->lock.
static _Bool allQueuesEmpty(struct Scheduler *s)
{
	for (int w = 0; w < s->workerCount; w++) {
		if (s->readyHead[w] != NULL) {
			return 0;
		}
	}
	return 1;
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


// Park the current fiber like schedulerSuspend, but atomically release the sync monitor
// STRIPE the fiber holds as part of the park — this closes the lost-wakeup window for the
// Smalltalk sync primitives. The caller MUST hold monitorLocks[stripe] (heapMonitorEnterStripe).
//
// Ordering & why no wakeup is lost: the caller holds `stripe`; we take `sched->lock`
// (always monitor→lock, never the reverse), publish FIBER_PARKING, THEN drop the
// stripe, THEN drop sched->lock and switch. A signaller must hold the SAME stripe to
// touch the waiter queue and call resumeId:, so it cannot run its resume until we have
// already published PARKING; schedulerResumeLocked then records parkPending and the run
// loop re-queues us instead of leaving us parked forever. Mirrors schedulerSleep's
// "register-the-wait and transition-to-PARKING under one lock hold" invariant. The stripe
// is passed in (computed once at monitorEnterOn:), never recomputed here.
void schedulerParkAndUnlockMonitorStripe(size_t stripe)
{
	if (gCurrent == NULL) {
		// No scheduler yet (bootstrap / main thread before schedulerRun): there is no
		// peer to hand off to, so just drop the stripe — matches schedulerSuspend's
		// gCurrent==NULL early-out (the "wait" becomes a no-op).
		heapMonitorExitStripe(CurrentThread.heap, stripe);
		return;
	}
	Fiber *self = gCurrent;
	schedLock();
	self->parkIntent = PARK_SUSPEND;
	self->parkPending = 0;
	self->state = FIBER_PARKING;
	heapMonitorExitStripe(CurrentThread.heap, stripe); // release stripe AFTER PARKING is published, still under sched->lock
	schedUnlock();
	yieldToScheduler(); // committed to SUSPENDED (or re-queued if a resume raced)
}

// Legacy no-arg park == stripe 0.
void schedulerParkAndUnlockMonitor(void)
{
	schedulerParkAndUnlockMonitorStripe(0);
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


void schedulerWaitFd(OsFd fd, int forWrite)
{
	Fiber *self = gCurrent;
	if (self == NULL || gEvents == NULL) {
		return;
	}

	// Arm readiness AND transition to PARKING under one lock hold (same lost-wakeup
	// reason as schedulerSleep: an already-ready fd could fire immediately on the poller).
	schedLock();
	osEventLoopArm(gEvents, fd, forWrite != 0, self->id);
	gArmedWaiters++;
	self->waitFd = fd;  // recorded so schedulerTerminate can disarm a killed waiter
	parkSelf(PARK_SUSPEND); // resumed by the event loop when the fd is ready
	self->waitFd = -1;  // woken normally: the event loop already accounted for us
}


void schedulerTerminate(size_t id)
{
	if (!gActive) {
		// Before the scheduler is running (e.g. an unhandled exception during
		// bootstrap) terminating "the process" means exiting the VM. No ensure:
		// cleanups run on this path (there is no fiber to unwind).
		exit(1);
	}
	if (gCurrent != NULL && gCurrent->id == id) {
		// Self-terminate: run the pending ensure:/ifCurtailed: cleanups on this
		// fiber's own (still live) stack before it is torn down. Runs before the
		// lock: cleanups are arbitrary Smalltalk and may allocate, yield or park.
		// Cleanups unlink as they run, so a terminate from inside one is bounded.
		runAllUnwindHandlers();
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
		osEventLoopDisarm(gEvents, fiber->waitFd);
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
	// Capture the victim's pending ensure:/ifCurtailed: chain before it is
	// unregistered; the cleanups run below, on THIS fiber, outside the lock.
	// (A READY fiber that never ran has an empty chain.)
	Value pendingUnwind = fiber->roots.unwindHandler;
	fiber->roots.unwindHandler = 0;
	unregisterFiber(fiber);
	schedUnlock();
	if (pendingUnwind != 0) {
		// The victim never runs again, so its cleanups run on the terminator.
		// The chain head is handle-protected inside before anything allocates;
		// no safepoint can intervene between the unlock and that handle.
		runUnwindHandlerChain(pendingUnwind);
	}
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

	uint64_t readyTags[64];
	// The wait blocks: count as safe so a peer's stop-the-world GC isn't held up, then
	// leave the blocked state (waiting out any in-progress STW) before mutating the
	// scheduler as an active mutator. Poller kicks (osEventLoopWake) are consumed
	// inside osEventLoopWait and only cause an early return with no tag.
	heapGcEnterBlocked(heap, &CurrentThread);
	int n = osEventLoopWait(gEvents, readyTags, 64, timeout);
	heapGcLeaveBlocked(heap, &CurrentThread);

	schedLock();
	for (int i = 0; i < n; i++) {
		if (gArmedWaiters > 0) {
			gArmedWaiters--;
		}
		schedulerResumeLocked((size_t) readyTags[i]);
	}
	wakeExpiredTimers(); // resumes via schedulerResumeLocked (lock held)
	s->pollerActive = 0;
	osCondBroadcast(&s->work); // wake idle workers for the freshly-ready fibers
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
			// Mark dirty UNDER sched->lock (running code may create young roots): the
			// shared dirty list must never be mutated unlocked. Was in runFiber (outside
			// the lock) — a data race vs register/unregister/scavenge that corrupts the
			// list and crashes a later scavenge. Idempotent (no-op if already dirty).
			schedulerMarkFiberDirty(fiber);
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
		if (s->runningCount == 0 && allQueuesEmpty(s) && gArmedWaiters == 0 && gTimerCount == 0) {
			// Nothing running, EVERY worker's queue empty, and nothing pending anywhere:
			// shut the whole pool down. (Must check all queues, not just ours — a fiber
			// could be ready on a peer worker's queue while we and it are both idle.)
			s->shutdown = 1;
			osCondBroadcast(&s->work);
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
		while (s->readyHead[gWorkerIndex] == NULL && !s->shutdown
		       && !((gArmedWaiters > 0 || gTimerCount > 0) && !s->pollerActive)) {
			osCondWait(&s->work, &s->lock);
		}
		s->idleWorkers--;
		schedUnlock();
		heapGcLeaveBlocked(heap, &CurrentThread); // wait out any in-progress STW before running
	}
}


// Argument to a helper worker thread: which heap to join and this worker's index
// (1..workerCount-1; index 0 is the schedulerRun thread).
typedef struct { Heap *heap; int index; } HelperArg;

// A spawned helper worker OS thread: set up its TLS to share the caller's heap and
// scheduler (mirrors parallelPrimWorker), then run the shared run loop.
static void schedulerHelperMain(void *arg)
{
	HelperArg *ha = (HelperArg *) arg;
	Heap *heap = ha->heap;
	int myIndex = ha->index;
	free(ha);
	struct Scheduler *s = heap->sched;

	memset(&CurrentThread, 0, sizeof(Thread));
	gWorkerIndex = myIndex; // this helper pops from readyHead[myIndex]
	CurrentThread.heap = heap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(heap, &CurrentThread);                      // register before any allocation
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler;
	CurrentThread.schedUnwindHandler = &CurrentThread.unwindHandler;
	// Handles are per-heap now (Handle.h): CurrentThread.heap == the shared heap, whose
	// handles are already populated — no TLS copy needed.
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
	// caches that the primary already set in schedulerInit BEFORE the spawn (a
	// happens-before edge), so helpers only READ them. Re-writing them races (TSan). We
	// still need this worker's OWN sigaltstack for the growable-stack SIGSEGV handler.
	osInstallSegvHandler(fiberSegvGrowCallback);

	schedulerRunWorker();

	heapEndMutator(heap, &CurrentThread); // deregister so a later GC never scans this dead thread
}


Value schedulerRun(void)
{
	struct Scheduler *s = curSched();
	Heap *heap = CurrentThread.heap;
	int helpers = s->workerCount - 1;

	gWorkerIndex = 0; // the schedulerRun thread is worker 0 (pops readyHead[0])

	OsThread *tids = NULL;
	if (helpers > 0) {
		tids = malloc((size_t) helpers * sizeof(OsThread));
		for (int i = 0; i < helpers; i++) {
			HelperArg *ha = malloc(sizeof(HelperArg));
			ha->heap = heap;
			ha->index = i + 1; // workers 1..workerCount-1
			osThreadSpawn(&tids[i], schedulerHelperMain, ha);
		}
	}

	schedulerRunWorker(); // the calling thread participates as a worker (index 0)

	if (helpers > 0) {
		// Our run loop returned (shutdown broadcast); wait for the helpers. Count as
		// safe while joining so a helper's final GC isn't blocked by us.
		heapGcEnterBlocked(heap, &CurrentThread);
		for (int i = 0; i < helpers; i++) {
			osThreadJoin(&tids[i]);
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
