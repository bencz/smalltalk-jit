#include "concurrency/Scheduler.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include "core/Thread.h"
#include "jit/Jit.h"
#include "memory/Roots.h"
#include "os/Os.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The state
// ---------------------------------------------------------------------------
//
// One ready queue, one registry, one running fiber. All of it is per-process
// because the scheduler is single-threaded (Scheduler.h); the day workers
// arrive this becomes per-worker and the registry stays shared for the
// collector.

typedef struct {
	Fiber *readyHead;
	Fiber *readyTail;
	// EVERY live fiber, running or not. The ready queue is not enough: the
	// collector has to reach suspended and sleeping ones too, and an id lookup
	// has to find a fiber that is on no queue at all.
	Fiber **all;
	size_t count;
	size_t capacity;
	Fiber *main;
	// The fiber that just switched off and whose park has NOT been committed.
	// The commit runs on the NEXT fiber's stack, never on the parking fiber's
	// own, which is the rule Fiber.h states: a peer must not be able to pop and
	// run a stack that is still switching off.
	Fiber *parking;
	size_t nextId;
	_Bool started;
} Scheduler;

static Scheduler gScheduler;


// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

static void registryAdd(Fiber *fiber)
{
	if (gScheduler.count == gScheduler.capacity) {
		size_t capacity = gScheduler.capacity == 0 ? 8 : gScheduler.capacity * 2;
		Fiber **grown = realloc(gScheduler.all, capacity * sizeof *grown);
		// Out of memory here is not recoverable in a useful way: the fiber
		// exists and its stack is reserved, and losing it from the registry
		// would lose it from the COLLECTOR, which is silent corruption rather
		// than a failed spawn.
		ASSERT(grown != NULL);
		gScheduler.all = grown;
		gScheduler.capacity = capacity;
	}
	gScheduler.all[gScheduler.count++] = fiber;
}


static void registryRemove(Fiber *fiber)
{
	for (size_t i = 0; i < gScheduler.count; i++) {
		if (gScheduler.all[i] == fiber) {
			gScheduler.all[i] = gScheduler.all[--gScheduler.count];
			return;
		}
	}
}


static Fiber *fiberWithId(size_t id)
{
	for (size_t i = 0; i < gScheduler.count; i++) {
		if (gScheduler.all[i]->id == id) {
			return gScheduler.all[i];
		}
	}
	return NULL;
}


// ---------------------------------------------------------------------------
// The ready queue
// ---------------------------------------------------------------------------
//
// FIFO, which is the whole fairness policy. Round-robin is what `yield` means
// to the code written against it: a fiber that yields goes to the BACK, so a
// tight `[...] whileTrue: [Processor yield]` cannot starve its peers.

static void readyPush(Fiber *fiber)
{
	fiber->queueNext = NULL;
	if (gScheduler.readyTail == NULL) {
		gScheduler.readyHead = fiber;
	} else {
		gScheduler.readyTail->queueNext = fiber;
	}
	gScheduler.readyTail = fiber;
}


static Fiber *readyPop(void)
{
	Fiber *fiber = gScheduler.readyHead;
	if (fiber == NULL) {
		return NULL;
	}
	gScheduler.readyHead = fiber->queueNext;
	if (gScheduler.readyHead == NULL) {
		gScheduler.readyTail = NULL;
	}
	fiber->queueNext = NULL;
	return fiber;
}


// ---------------------------------------------------------------------------
// Sleeping
// ---------------------------------------------------------------------------
//
// A sleeper is SUSPENDED with a deadline, not a separate state: it is off the
// ready queue exactly like any other suspended fiber, and what distinguishes it
// is a wake time nothing else reads. Adding a FIBER_SLEEPING state would have
// every switch site learn about it for no gain.
//
// The deadline lives HERE and not in the Fiber, because it is scheduling policy
// and Fiber.h is the machine layer.

typedef struct {
	Fiber *fiber;
	int64_t wakeAtNanos;
} Sleeper;

static Sleeper *gSleepers;
static size_t gSleeperCount;
static size_t gSleeperCapacity;


static void sleepersAdd(Fiber *fiber, int64_t wakeAtNanos)
{
	if (gSleeperCount == gSleeperCapacity) {
		size_t capacity = gSleeperCapacity == 0 ? 8 : gSleeperCapacity * 2;
		Sleeper *grown = realloc(gSleepers, capacity * sizeof *grown);
		ASSERT(grown != NULL);
		gSleepers = grown;
		gSleeperCapacity = capacity;
	}
	gSleepers[gSleeperCount].fiber = fiber;
	gSleepers[gSleeperCount].wakeAtNanos = wakeAtNanos;
	gSleeperCount++;
}


static void sleepersDrop(Fiber *fiber)
{
	for (size_t i = 0; i < gSleeperCount; i++) {
		if (gSleepers[i].fiber == fiber) {
			gSleepers[i] = gSleepers[--gSleeperCount];
			return;
		}
	}
}


// Move every sleeper whose deadline has passed back to the ready queue.
// Answers the earliest remaining deadline, or 0 when nobody is sleeping.
static int64_t sleepersWake(void)
{
	int64_t now = osMonotonicNanos();
	int64_t earliest = 0;
	for (size_t i = 0; i < gSleeperCount;) {
		if (gSleepers[i].wakeAtNanos <= now) {
			Fiber *fiber = gSleepers[i].fiber;
			gSleepers[i] = gSleepers[--gSleeperCount];
			if (fiber->state == FIBER_SUSPENDED) {
				fiber->state = FIBER_READY;
				readyPush(fiber);
			}
			continue; // the slot now holds a different sleeper
		}
		if (earliest == 0 || gSleepers[i].wakeAtNanos < earliest) {
			earliest = gSleepers[i].wakeAtNanos;
		}
		i++;
	}
	return earliest;
}


// ---------------------------------------------------------------------------
// Thread state, which belongs to the FIBER and not to the OS thread
// ---------------------------------------------------------------------------
//
// Handle scopes, the native frame chain and the two handler chains are all
// per-ACTIVATION, and a fiber is a stack of activations. Leaving them in the
// Thread across a switch would hand the next fiber the previous one's handle
// scopes and its on:do: chain, so an exception raised in one fiber would find a
// handler installed by another.

static void saveThreadStateInto(Fiber *fiber)
{
	fiber->roots.handleScopes = CurrentThread.handleScopes;
	fiber->roots.stackFramesTail = CurrentThread.stackFramesTail;
	fiber->roots.context = CurrentThread.context;
	fiber->roots.exceptionHandler = CurrentThread.exceptionHandler;
	fiber->roots.unwindHandler = CurrentThread.unwindHandler;
	fiber->roots.compiledFrames = CurrentThread.compiledFrames;
	fiber->roots.unwinds = CurrentThread.unwinds;
	fiber->roots.homeToken = CurrentThread.homeToken;
}


static void restoreThreadStateFrom(Fiber *fiber)
{
	CurrentThread.handleScopes = fiber->roots.handleScopes;
	CurrentThread.stackFramesTail = fiber->roots.stackFramesTail;
	CurrentThread.context = fiber->roots.context;
	CurrentThread.exceptionHandler = fiber->roots.exceptionHandler;
	CurrentThread.unwindHandler = fiber->roots.unwindHandler;
	CurrentThread.compiledFrames = fiber->roots.compiledFrames;
	CurrentThread.unwinds = fiber->roots.unwinds;
	CurrentThread.homeToken = fiber->roots.homeToken;
	// nextHomeToken is NOT restored: it is the process-wide minter, and the
	// reason is written at FiberRoots in concurrency/Fiber.h.
}


// ---------------------------------------------------------------------------
// Switching
// ---------------------------------------------------------------------------

// Finish the park of whichever fiber handed us control.
//
// It runs HERE, after the switch, and that ordering is the contract in
// Fiber.h: while a fiber is still executing fiberSwitchAsm it is neither on its
// own stack nor off it, and putting it on the ready queue in that window would
// let the next fiber pop it and switch into a half-saved context.
static void commitPendingPark(void)
{
	Fiber *parked = gScheduler.parking;
	if (parked == NULL) {
		return;
	}
	gScheduler.parking = NULL;
	if (parked->state != FIBER_PARKING) {
		return; // already resolved, e.g. terminated while parking
	}
	if (parked->parkIntent == PARK_SUSPEND) {
		parked->state = FIBER_SUSPENDED;
	} else {
		parked->state = FIBER_READY;
		readyPush(parked);
	}
	parked->parkIntent = PARK_NONE;
}


// Reclaim fibers that ran to completion. Done from a RUNNING fiber's stack,
// never from the finished fiber's own: freeing a stack you are standing on is
// the one thing this cannot do.
static void reapFinished(void)
{
	for (size_t i = 0; i < gScheduler.count;) {
		Fiber *fiber = gScheduler.all[i];
		if (fiber->state == FIBER_DONE && fiber != fiberCurrent()) {
			gScheduler.all[i] = gScheduler.all[--gScheduler.count];
			fiberDestroy(fiber);
			continue;
		}
		i++;
	}
}


// Switch from the running fiber to `next`, which must be READY or fresh.
static void switchTo(Fiber *next)
{
	Fiber *self = fiberCurrent();
	next->state = FIBER_RUNNING;
	next->resumer = self;
	saveThreadStateInto(self);
	fiberSwitch(self, next);
	// Back on our own stack: everything below runs for the fiber that was just
	// switched INTO, which is us again.
	restoreThreadStateFrom(self);
	commitPendingPark();
	reapFinished();
}


// Park the running fiber with `intent` and give the CPU to someone else.
// Answers 0 when there was nobody to give it to.
static _Bool parkAndSwitch(ParkIntent intent)
{
	Fiber *self = fiberCurrent();
	Fiber *next = readyPop();
	if (next == NULL) {
		return 0;
	}
	self->state = FIBER_PARKING;
	self->parkIntent = intent;
	gScheduler.parking = self;
	switchTo(next);
	self->state = FIBER_RUNNING;
	return 1;
}


// ---------------------------------------------------------------------------
// The entry trampoline
// ---------------------------------------------------------------------------

// What a spawned fiber starts on, reached from Fiber.c's own trampoline
// through cEntry/cArg.
//
// What a spawned fiber starts on. It evaluates the block and then has to hand
// control back WITHOUT returning, because there is nothing to return to: the
// bottom of this stack was primed by fiberPrimeStack, not called from anywhere.
static void runSpawnedFiber(void *arg)
{
	Fiber *self = arg;
	// A fresh fiber starts with EMPTY VM state, not with whatever the spawner
	// happened to be holding. Its handle scopes and frame chain begin here, and
	// its handler chains are empty, which is what makes an unhandled exception
	// in a spawned process reach the default action instead of a handler that
	// belongs to another fiber.
	restoreThreadStateFrom(self);
	// AND THE PARK OF WHOEVER GAVE US THE CPU. A fresh fiber does not return
	// from fiberSwitch -- it starts here -- so the commit that switchTo does on
	// its return path never runs for this hand-off. Without it the fiber that
	// yielded to us stays FIBER_PARKING for good: off the ready queue, never
	// suspended, never scheduled again. Measured as a `fork` round-robin that
	// dropped the forking fiber after its first turn.
	commitPendingPark();
	reapFinished();

	if (valueTypeOf(self->entryBlock, VALUE_POINTER)) {
		jitSendUnary(self->entryBlock, "value", NULL);
	}

	// Done. Drop the block so a finished fiber stops keeping its closure, and
	// everything the closure captured, alive until it is reaped.
	self->entryBlock = tagInt(0);
	self->state = FIBER_DONE;

	// Hand control on and never come back. There is no park to commit for this
	// fiber -- it is DONE, not PARKING -- and reaping happens on the next
	// fiber's stack, which is why this switch is the last thing here.
	Fiber *next = readyPop();
	if (next == NULL) {
		next = gScheduler.main;
	}
	next->state = FIBER_RUNNING;
	saveThreadStateInto(self);
	fiberSwitch(self, next);
	// Unreachable: nothing switches into a DONE fiber.
	ASSERT(0);
}


// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

void schedulerInit(void)
{
	if (gScheduler.started) {
		return;
	}
	gScheduler.started = 1;
	gScheduler.nextId = 1;
	// 8 MB reserved per stack with 64 KB committed: the reservation is address
	// space, not memory, and the window grows on fault (Fiber.h).
	fiberInitStacks(8u << 20, 64u << 10);
	fiberInstallGrowthHandler();

	Fiber *main = calloc(1, sizeof *main);
	ASSERT(main != NULL);
	fiberAdoptCurrentStack(main);
	main->id = gScheduler.nextId++;
	main->state = FIBER_RUNNING;
	main->entryBlock = tagInt(0);
	main->process = tagInt(0);
	gScheduler.main = main;
	registryAdd(main);
}


size_t schedulerCurrentId(void)
{
	schedulerInit();
	Fiber *self = fiberCurrent();
	return self == NULL ? 0 : self->id;
}


size_t schedulerSpawn(Value block)
{
	schedulerInit();
	Fiber *fiber = fiberCreate(runSpawnedFiber, NULL);
	if (fiber == NULL) {
		return 0;
	}
	fiber->cArg = fiber; // the trampoline needs its own Fiber, not the spawner's
	fiber->id = gScheduler.nextId++;
	fiber->entryBlock = block;
	fiber->process = tagInt(0);
	// SUSPENDED, not ready: `Block>>newProcess` promises a process that does not
	// run until it is sent #resume, and `fork` is newProcess plus resume.
	fiber->state = FIBER_SUSPENDED;
	registryAdd(fiber);
	return fiber->id;
}


void schedulerYield(void)
{
	schedulerInit();
	sleepersWake();
	parkAndSwitch(PARK_YIELD); // nobody else ready: carry on, having done nothing
}


void schedulerSuspendCurrent(void)
{
	schedulerInit();
	Fiber *self = fiberCurrent();
	if (self == gScheduler.main && gScheduler.readyHead == NULL
			&& gSleeperCount == 0) {
		// Suspending the last runnable fiber would stop the program with no way
		// back. Refusing is not a policy choice: there is no fiber left that
		// could ever resume this one.
		return;
	}
	while (!parkAndSwitch(PARK_SUSPEND)) {
		// Nothing ready, but sleepers may be due. If none of them can ever wake
		// us either, this would spin forever, so it waits for the earliest
		// deadline instead of burning the CPU.
		int64_t earliest = sleepersWake();
		if (gScheduler.readyHead != NULL) {
			continue;
		}
		if (earliest == 0) {
			return; // nobody sleeping and nobody ready: see above
		}
		int64_t remaining = earliest - osMonotonicNanos();
		if (remaining > 0) {
			osSleepNanos(remaining);
		}
	}
}


void schedulerSleep(int64_t micros)
{
	schedulerInit();
	if (micros <= 0) {
		schedulerYield();
		return;
	}
	Fiber *self = fiberCurrent();
	sleepersAdd(self, osMonotonicNanos() + micros * 1000);
	// The sleeper is SUSPENDED; sleepersWake is what makes it ready again, and
	// it runs from schedulerYield and from the wait loop below.
	if (!parkAndSwitch(PARK_SUSPEND)) {
		// The only runnable fiber: there is nobody to switch to, so this is an
		// ordinary blocking sleep. It is still correct -- no other fiber could
		// have made progress -- and it keeps a lone `Delay wait` from spinning.
		sleepersDrop(self);
		self->state = FIBER_RUNNING;
		osSleepNanos(micros * 1000);
	}
}


_Bool schedulerResume(size_t id)
{
	schedulerInit();
	Fiber *fiber = fiberWithId(id);
	if (fiber == NULL || fiber->state != FIBER_SUSPENDED) {
		return 0;
	}
	sleepersDrop(fiber); // resuming early cancels a sleep
	fiber->state = FIBER_READY;
	readyPush(fiber);
	return 1;
}


_Bool schedulerTerminate(size_t id)
{
	schedulerInit();
	Fiber *fiber = fiberWithId(id);
	if (fiber == NULL || fiber == fiberCurrent() || fiber == gScheduler.main) {
		return 0;
	}
	// Unlink it from wherever it is waiting, then drop it. Its stack is
	// reclaimed without running anything on it: an `ensure:` block in a
	// terminated fiber would have to be run ON that stack, which is the
	// unwinder's job and is not implemented here. Said out loud because the
	// difference is observable.
	sleepersDrop(fiber);
	if (fiber->state == FIBER_READY) {
		Fiber **link = &gScheduler.readyHead;
		Fiber *previous = NULL;
		while (*link != NULL && *link != fiber) {
			previous = *link;
			link = &(*link)->queueNext;
		}
		if (*link == fiber) {
			*link = fiber->queueNext;
			if (gScheduler.readyTail == fiber) {
				gScheduler.readyTail = previous;
			}
		}
	}
	registryRemove(fiber);
	fiberDestroy(fiber);
	return 1;
}


// ---------------------------------------------------------------------------
// The collector's view
// ---------------------------------------------------------------------------

// The strong definition of the seam declared in memory/Roots.h. Linking this
// file is what turns the weak no-op there into a real walk.
void rootsVisitFibers(RootVisitor visit, void *ctx)
{
	Fiber *running = fiberCurrent();
	for (size_t i = 0; i < gScheduler.count; i++) {
		Fiber *fiber = gScheduler.all[i];
		// The RUNNING fiber is skipped: its live state is in CurrentThread and
		// the per-mutator scan already has it. Its FiberRoots copy is stale, and
		// the reason that matters is written at rootsVisitFibers in Roots.h.
		if (fiber == running) {
			continue;
		}
		fiberVisitRoots(fiber, visit, ctx);

		// The chains, through a THREAD-SHAPED VIEW of this fiber rather than a
		// second set of walkers. handlesVisitRoots, rootsVisitNativeFrames and
		// rootsVisitUnwindRecords all read exactly these fields out of a Thread,
		// and giving them a view is what keeps ONE implementation of each walk.
		// A second copy here is the drift this project keeps paying for, and a
		// frame walker is the last place to have two of.
		Thread view;
		memset(&view, 0, sizeof view);
		view.handleScopes = fiber->roots.handleScopes;
		view.compiledFrames = fiber->roots.compiledFrames;
		view.unwinds = fiber->roots.unwinds;
		handlesVisitRoots(&view, visit, ctx);
		rootsVisitNativeFrames(&view, visit, ctx);
		rootsVisitUnwindRecords(&view, visit, ctx);
	}
}
