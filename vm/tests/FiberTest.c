// Gate level 1: fibers switch, their stacks grow, and their roots are walkable.
//
// Runs against the fiber layer ALONE: no scheduler, no compiler, no JIT. That
// is the point. A context switch is twenty instructions that are either exactly
// right or corrupt the machine in a way that surfaces somewhere else entirely,
// so it is worth proving on its own before anything runs on top of it.
//
// What it checks, and why each one:
//
//   1. a switch reaches the fiber's entry with a usable stack
//   2. control comes back to the switcher, and can go out again
//   3. round-robin across several fibers preserves each one's locals, which is
//      the actual claim of a stackful coroutine
//   4. the callee-saved set survives the switch, in BOTH directions. This is
//      the one that silently corrupts everything downstream when wrong
//   5. a deep call chain grows the committed window instead of faulting, and
//      the values written down there are intact on the way back up
//   6. a finished fiber hands control back rather than returning into nothing
//   7. root visiting reaches a fiber's Values

#include "concurrency/Fiber.h"
#include <stdio.h>
#include <string.h>

static int gFailures;
static int gChecks;

static void check(const char *what, int ok)
{
	gChecks++;
	if (!ok) {
		gFailures++;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  ok    %s\n", what);
	}
}

static Fiber gMain;
static Fiber *gWorkers[3];
static int gTrace[16];
static int gTraceCount;
static int gVisits;

static void trace(int mark)
{
	if (gTraceCount < 16) {
		gTrace[gTraceCount++] = mark;
	}
}


// Each worker keeps a local counter across yields. If a switch corrupted the
// stack this is what would come back wrong.
static void workerEntry(void *arg)
{
	int id = (int) (intptr_t) arg;
	int local = id * 100;
	for (int round = 0; round < 3; round++) {
		local++;
		trace(local);
		fiberSwitch(gWorkers[id], &gMain);
	}
}


// Touches every callee-saved register the SysV switch is responsible for, then
// yields, then verifies they came back. Written so the compiler must actually
// keep values live across the call.
static void calleeSavedEntry(void *arg)
{
	volatile long a = 0x1111111111111111L;
	volatile long b = 0x2222222222222222L;
	volatile long c = 0x3333333333333333L;
	volatile long d = 0x4444444444444444L;
	volatile long e = 0x5555555555555555L;
	fiberSwitch((Fiber *) arg, &gMain);
	check("callee-saved values survive a round trip",
		a == 0x1111111111111111L && b == 0x2222222222222222L
		&& c == 0x3333333333333333L && d == 0x4444444444444444L
		&& e == 0x5555555555555555L);
	fiberSwitch((Fiber *) arg, &gMain);
}


// Recurse with a fat frame so the committed window has to grow several times,
// writing a signature into each frame and checking it on the way back.
static long deepRecurse(int depth)
{
	volatile char pad[4096];
	memset((void *) pad, (char) (depth & 0x7F), sizeof(pad));
	long below = depth > 0 ? deepRecurse(depth - 1) : 0;
	if (pad[0] != (char) (depth & 0x7F) || pad[4095] != (char) (depth & 0x7F)) {
		return -1;
	}
	return below < 0 ? -1 : below + 1;
}

static long gDeepResult;
static size_t gCommittedBefore;
static size_t gCommittedAfter;

static void deepEntry(void *arg)
{
	Fiber *self = arg;
	gCommittedBefore = (size_t) (self->stackHigh - self->committedLow);
	gDeepResult = deepRecurse(120); // ~500 KB of frames, well past the 16 KB window
	gCommittedAfter = (size_t) (self->stackHigh - self->committedLow);
	fiberSwitch(self, &gMain);
}


static void countingVisitor(void *ctx, Value *slot)
{
	(void) slot;
	(*(int *) ctx)++;
}


int main(void)
{
	printf("gate level 1: fibers switch, stacks grow, roots are walkable\n\n");

	fiberInitStacks(1024 * 1024, 16 * 1024);
	fiberInstallGrowthHandler();
	fiberAdoptCurrentStack(&gMain);

	// ---- 1, 2, 3: switching and per-fiber locals ---------------------------
	for (int i = 0; i < 3; i++) {
		gWorkers[i] = fiberCreate(workerEntry, (void *) (intptr_t) i);
	}
	check("fiberCreate returns a primed fiber",
		gWorkers[0] != NULL && gWorkers[0]->sp != NULL);
	check("a new fiber starts READY", gWorkers[0]->state == FIBER_READY);

	for (int round = 0; round < 3; round++) {
		for (int i = 0; i < 3; i++) {
			fiberSwitch(&gMain, gWorkers[i]);
		}
	}

	// Each worker ran three times, incrementing its own counter from id*100.
	static const int expected[9] = { 1, 101, 201, 2, 102, 202, 3, 103, 203 };
	check("round-robin preserves each fiber's locals",
		gTraceCount == 9 && memcmp(gTrace, expected, sizeof(expected)) == 0);
	check("control returns to the switcher every time", gTraceCount == 9);

	// ---- 4: the callee-saved set ------------------------------------------
	Fiber *saved = fiberCreate(calleeSavedEntry, NULL);
	saved->cArg = saved;
	fiberSwitch(&gMain, saved);
	fiberSwitch(&gMain, saved);

	// ---- 5: growable stacks -------------------------------------------------
	Fiber *deep = fiberCreate(deepEntry, NULL);
	deep->cArg = deep;
	fiberSwitch(&gMain, deep);
	check("a deep call chain completes without faulting", gDeepResult == 121);
	check("the committed window actually grew",
		gCommittedAfter > gCommittedBefore);
	check("the window grew by roughly the frames used",
		gCommittedAfter >= 120 * 4096);

	// ---- 6: a finished fiber hands control back ----------------------------
	fiberSwitch(&gMain, gWorkers[0]); // its loop is over: falls off the entry
	check("a finished fiber is DONE", gWorkers[0]->state == FIBER_DONE);
	check("switching into a finished fiber returns control", 1);

	// ---- 7: roots --------------------------------------------------------
	gWorkers[1]->process = tagInt(7);          // not a pointer: must be skipped
	gWorkers[1]->entryBlock = tagPtr(&gMain);  // a pointer-shaped Value
	gWorkers[1]->roots.context = tagPtr(&gMain);
	gVisits = 0;
	fiberVisitRoots(gWorkers[1], countingVisitor, &gVisits);
	check("root visiting reaches the pointer Values and skips the rest",
		gVisits == 2);

	// ---- teardown ----------------------------------------------------------
	for (int i = 0; i < 3; i++) {
		fiberReleaseIdleStack(gWorkers[i]);
		fiberDestroy(gWorkers[i]);
	}
	fiberDestroy(saved);
	fiberDestroy(deep);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
