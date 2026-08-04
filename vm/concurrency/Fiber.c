#include "concurrency/Fiber.h"
#include "core/Assert.h"
#include "core/Tls.h"
#include "os/Os.h"
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// PORT_ME(stack-direction): everything below assumes the stack grows toward
// LOWER addresses. Priming at the high end, the guard floor at the base,
// fiberGrowStack committing downward toward the fault, fiberReleaseIdleStack
// reclaiming [committedLow, sp). Every currently supported target does; an
// upward-stack port reworks this file rather than flipping a flag.

#define KB 1024
#define MB (1024 * 1024)

// Reserved per fiber: ADDRESS SPACE ONLY, no RAM and no overcommit charge.
// 100k fibers at 1 MB of reservation is 100 GB of address space, which is free,
// and a few hundred MB of real pages, which is not. Getting this backwards is
// what makes fiber-per-connection servers impossible.
#define DEFAULT_RESERVATION (1 * MB)
#define DEFAULT_COMMIT (16 * KB)
// Pages kept unreachable at the base, so runaway recursion faults rather than
// growing quietly until the machine dies.
#define STACK_FLOOR_PAGES 2

static PER_ISOLATE size_t gReservationBytes;
static PER_ISOLATE size_t gCommitBytes;
static PER_ISOLATE size_t gPageSize;
// The fiber this OS thread is executing right now. The switch keeps it current;
// the growth handler reads it to learn whose stack just faulted.
static PER_ISOLATE Fiber *gCurrentFiber;
static PER_ISOLATE size_t gNextFiberId;


void fiberInitStacks(size_t reservationBytes, size_t initialCommitBytes)
{
	gPageSize = (size_t) osPageSize();
	gReservationBytes = reservationBytes != 0 ? reservationBytes : DEFAULT_RESERVATION;
	gCommitBytes = initialCommitBytes != 0 ? initialCommitBytes : DEFAULT_COMMIT;
	// Every span handed to osPageCommit must be page-aligned.
	gReservationBytes = (gReservationBytes + gPageSize - 1) & ~(gPageSize - 1);
	gCommitBytes = (gCommitBytes + gPageSize - 1) & ~(gPageSize - 1);
	gNextFiberId = 1;
}


Fiber *fiberCurrent(void)
{
	return gCurrentFiber;
}


// Where a primed stack lands on its FIRST switch. It runs on the new fiber's
// own stack, which is why it must never return: there is no frame under it.
static void fiberTrampoline(void)
{
	Fiber *self = gCurrentFiber;
	self->state = FIBER_RUNNING;
	if (self->cEntry != NULL) {
		self->cEntry(self->cArg);
	}
	self->state = FIBER_DONE;
	// Hand control back to whoever resumed us, and keep handing it back if a
	// broken scheduler resumes a finished fiber: hanging visibly beats
	// returning into a frame that does not exist.
	for (;;) {
		Fiber *resumer = self->resumer;
		ASSERT(resumer != NULL);
		gCurrentFiber = resumer;
		fiberSwitchAsm(&self->sp, resumer->sp);
	}
}


Fiber *fiberCreate(FiberEntry entry, void *arg)
{
	if (gPageSize == 0) {
		fiberInitStacks(0, 0);
	}
	Fiber *fiber = calloc(1, sizeof(Fiber));
	ASSERT(fiber != NULL);

	uint8_t *reservation = osPageReserve(gReservationBytes);
	ASSERT(reservation != NULL);
	fiber->reservation = reservation;
	fiber->reservationSize = gReservationBytes;
	fiber->stackHigh = reservation + gReservationBytes;
	fiber->committedLow = fiber->stackHigh - gCommitBytes;
	fiber->floor = reservation + STACK_FLOOR_PAGES * gPageSize;
	_Bool committed = osPageCommit(fiber->committedLow, gCommitBytes);
	ASSERT(committed);

	fiber->state = FIBER_READY;
	fiber->parkIntent = PARK_NONE;
	fiber->homeWorker = -1;
	fiber->cEntry = entry;
	fiber->cArg = arg;
	fiber->id = gNextFiberId++;
	fiber->sp = fiberPrimeStack(fiber->stackHigh, fiberTrampoline);
	return fiber;
}


// Bind the OS thread's OWN context to a Fiber, so the scheduler can switch away
// from it and back through the same mechanism as everything else. Its stack is
// the real thread stack: a NULL reservation marks it as not ours to grow or
// free.
void fiberAdoptCurrentStack(Fiber *fiber)
{
	memset(fiber, 0, sizeof(*fiber));
	fiber->state = FIBER_RUNNING;
	fiber->homeWorker = -1;
	gCurrentFiber = fiber;
}


void fiberDestroy(Fiber *fiber)
{
	if (fiber->reservation != NULL) {
		osPageFree(fiber->reservation, fiber->reservationSize);
		fiber->reservation = NULL;
	}
	free(fiber);
}


void fiberReleaseIdleStack(Fiber *fiber)
{
	// Hand back the pages below the parked stack pointer. The mapping stays
	// valid and re-faults zero-filled, so a fiber that parks deep and resumes
	// shallow costs nothing to keep alive.
	uint8_t *sp = fiber->sp;
	if (fiber->reservation == NULL || sp == NULL || sp <= fiber->committedLow) {
		return;
	}
	uintptr_t low = ((uintptr_t) fiber->committedLow + gPageSize - 1) & ~(gPageSize - 1);
	uintptr_t high = (uintptr_t) sp & ~(gPageSize - 1);
	if (high > low) {
		osPageRelease((void *) low, (size_t) (high - low));
	}
}


void fiberSwitch(Fiber *from, Fiber *to)
{
	ASSERT(from != to);
	to->resumer = from;
	gCurrentFiber = to;
	to->state = FIBER_RUNNING;
	fiberSwitchAsm(&from->sp, to->sp);
	// Control is back in `from`. Whoever switched here may not be the fiber we
	// switched to, so gCurrentFiber is restored rather than assumed.
	gCurrentFiber = from;
	from->state = FIBER_RUNNING;
}


// ---------------------------------------------------------------------------
// Growable stacks
// ---------------------------------------------------------------------------

int fiberGrowStack(Fiber *fiber, uintptr_t faultAddress)
{
	if (fiber == NULL || fiber->reservation == NULL) {
		return 0; // an adopted OS-thread stack: not ours to grow
	}
	uint8_t *address = (uint8_t *) faultAddress;
	// Above the window means the fault was never about growth. Below the floor
	// means genuine overflow. NEITHER may be swallowed: a swallowed fault here
	// turns a wild write into a bug that looks like a stack problem forever.
	if (address >= fiber->committedLow || address < fiber->floor) {
		return 0;
	}
	// Commit down to the faulting page plus a page of headroom, so a deep call
	// chain does not fault once per page on the way down.
	uintptr_t page = (uintptr_t) address & ~(gPageSize - 1);
	uintptr_t floor = (uintptr_t) fiber->floor;
	uintptr_t low = page > floor + gPageSize ? page - gPageSize : floor;
	size_t bytes = (size_t) ((uintptr_t) fiber->committedLow - low);
	if (!osPageCommit((void *) low, bytes)) {
		return 0;
	}
	fiber->committedLow = (uint8_t *) low;
	return 1;
}


static void growthHandler(int signal, siginfo_t *info, void *context)
{
	(void) context;
	if (fiberGrowStack(gCurrentFiber, (uintptr_t) info->si_addr)) {
		return; // the faulting instruction retries and now succeeds
	}
	// Not a growth fault. Restore the default action and re-raise, so the
	// process dies where it actually broke, with a usable core, instead of
	// spinning in the handler.
	struct sigaction fallback;
	memset(&fallback, 0, sizeof(fallback));
	fallback.sa_handler = SIG_DFL;
	sigaction(signal, &fallback, NULL);
	raise(signal);
}


void fiberInstallGrowthHandler(void)
{
	// The handler runs precisely when a stack could not grow, so it must not
	// run ON that stack. The alternate signal stack is not an optimization
	// here: without it the handler never gets to execute at all.
	static PER_ISOLATE char altStack[64 * KB];
	stack_t alt;
	alt.ss_sp = altStack;
	alt.ss_size = sizeof(altStack);
	alt.ss_flags = 0;
	sigaltstack(&alt, NULL);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = growthHandler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
}


// ---------------------------------------------------------------------------
// Roots
// ---------------------------------------------------------------------------

void fiberVisitRoots(Fiber *fiber, RootVisitor visit, void *ctx)
{
	if (valueTypeOf(fiber->entryBlock, VALUE_POINTER)) {
		visit(ctx, &fiber->entryBlock);
	}
	if (valueTypeOf(fiber->process, VALUE_POINTER)) {
		visit(ctx, &fiber->process);
	}
	if (valueTypeOf(fiber->roots.context, VALUE_POINTER)) {
		visit(ctx, &fiber->roots.context);
	}
	if (valueTypeOf(fiber->roots.exceptionHandler, VALUE_POINTER)) {
		visit(ctx, &fiber->roots.exceptionHandler);
	}
	if (valueTypeOf(fiber->roots.unwindHandler, VALUE_POINTER)) {
		visit(ctx, &fiber->roots.unwindHandler);
	}
	// The DEEP root set is the native frames on this fiber's stack, and it is
	// walked by whichever engine owns frames (memory/Roots.h). With no engine
	// linked that is the weak no-op, which is exactly what lets this layer be
	// proved before a JIT exists.
}
