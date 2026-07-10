#include "Safepoint.h"
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// One process-global handshake. `gSafepointRequested` is the lockless fast-path
// flag; everything else is guarded by gLock and coordinated through gCond:
//   - a mutator parking at a poll broadcasts to wake a waiting coordinator;
//   - the coordinator waits until every registered thread is safe;
//   - safepointEnd() clears the flag and broadcasts to release the parked ones.
// A single cond serves both directions (spurious wakeups just re-check state).
// ---------------------------------------------------------------------------

int gSafepointRequested = 0;

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gCond = PTHREAD_COND_INITIALIZER;
static SafepointThread *gThreads = NULL; // intrusive list, guarded by gLock


void safepointInit(void)
{
	// The statically-initialised mutex/cond are ready; reset dynamic state so a
	// self-test (or a re-init) starts clean.
	pthread_mutex_lock(&gLock);
	gThreads = NULL;
	__atomic_store_n(&gSafepointRequested, 0, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&gLock);
}


// Park at a safepoint until it is released. Caller may or may not hold gLock:
// the two internal callers pass ownsLock so we don't re-lock.
static void parkUntilReleased(SafepointThread *thread)
{
	thread->atSafepoint = 1;
	pthread_cond_broadcast(&gCond); // a waiting coordinator may now be satisfied
	while (__atomic_load_n(&gSafepointRequested, __ATOMIC_ACQUIRE)) {
		pthread_cond_wait(&gCond, &gLock);
	}
	thread->atSafepoint = 0;
}


void safepointRegister(SafepointThread *thread)
{
	pthread_mutex_lock(&gLock);
	thread->state = SP_ACTIVE;
	thread->atSafepoint = 0;
	thread->next = gThreads;
	gThreads = thread;
	// If a safepoint is already in progress, this fresh thread must not run
	// managed code yet — park it immediately.
	if (__atomic_load_n(&gSafepointRequested, __ATOMIC_ACQUIRE)) {
		parkUntilReleased(thread);
	}
	pthread_mutex_unlock(&gLock);
}


void safepointUnregister(SafepointThread *thread)
{
	pthread_mutex_lock(&gLock);
	SafepointThread **link = &gThreads;
	while (*link != NULL && *link != thread) {
		link = &(*link)->next;
	}
	if (*link == thread) {
		*link = thread->next;
	}
	// A coordinator may have been waiting on this thread; it is gone now.
	pthread_cond_broadcast(&gCond);
	pthread_mutex_unlock(&gLock);
}


void safepointBlock(SafepointThread *thread)
{
	pthread_mutex_lock(&gLock);
	if (__atomic_load_n(&gSafepointRequested, __ATOMIC_ACQUIRE)) {
		parkUntilReleased(thread);
	}
	pthread_mutex_unlock(&gLock);
}


void safepointEnterBlocked(SafepointThread *thread)
{
	pthread_mutex_lock(&gLock);
	thread->state = SP_BLOCKED;
	// Now counts as safe; a coordinator waiting on us can proceed.
	pthread_cond_broadcast(&gCond);
	pthread_mutex_unlock(&gLock);
}


void safepointLeaveBlocked(SafepointThread *thread)
{
	pthread_mutex_lock(&gLock);
	// Cannot resume mutating while the world is stopped.
	while (__atomic_load_n(&gSafepointRequested, __ATOMIC_ACQUIRE)) {
		pthread_cond_wait(&gCond, &gLock);
	}
	thread->state = SP_ACTIVE;
	pthread_mutex_unlock(&gLock);
}


// gLock held. True once every registered thread (except `exclude`) is parked at
// a safepoint or blocked in native code.
static int allSafe(SafepointThread *exclude)
{
	for (SafepointThread *t = gThreads; t != NULL; t = t->next) {
		if (t == exclude) {
			continue;
		}
		if (!t->atSafepoint && t->state != SP_BLOCKED) {
			return 0;
		}
	}
	return 1;
}


void safepointBegin(SafepointThread *exclude)
{
	pthread_mutex_lock(&gLock);
	__atomic_store_n(&gSafepointRequested, 1, __ATOMIC_RELEASE);
	while (!allSafe(exclude)) {
		pthread_cond_wait(&gCond, &gLock);
	}
	// World stopped. Release the lock: correctness now comes from every other
	// mutator being parked, not from holding gLock, so the collection can run
	// (and can itself take other locks) without contending here.
	pthread_mutex_unlock(&gLock);
}


void safepointEnd(void)
{
	pthread_mutex_lock(&gLock);
	__atomic_store_n(&gSafepointRequested, 0, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&gCond);
	pthread_mutex_unlock(&gLock);
}


// ---- self-test ------------------------------------------------------------
// Pure C (no heap/image): N worker threads run "managed work" + poll, and one
// coordinator repeatedly stops the world and mutates a flag that NO worker may
// ever observe set while it is running. If the handshake is broken (a worker
// runs during STW), gRaces goes non-zero and the test fails.

#define SP_TEST_WORKERS 6
#define SP_TEST_STOPS   3000

static volatile int gInSafepoint = 0; // set only while the world is stopped
static long gRaces = 0;               // worker saw itself active during STW
static long gWorkDone = 0;            // total worker iterations (progress proof)
static int gStopWorkers = 0;

static void *safepointTestWorker(void *arg)
{
	SafepointThread me;
	unsigned seed = (unsigned) (uintptr_t) arg * 2654435761u + 1u;
	long work = 0;
	long races = 0;

	safepointRegister(&me);
	while (!__atomic_load_n(&gStopWorkers, __ATOMIC_ACQUIRE)) {
		safepointPoll(&me);
		// --- managed-work section: must never overlap a safepoint ---
		if (gInSafepoint) {
			races++;
		}
		volatile long spin = 0;
		for (int i = 0; i < 40; i++) {
			spin += i;
		}
		work++;
		// --- end managed-work section ---

		// Occasionally simulate a blocking syscall.
		seed = seed * 1103515245u + 12345u;
		if (((seed >> 16) & 0x3f) == 0) {
			safepointEnterBlocked(&me);
			volatile long s = 0;
			for (int i = 0; i < 100; i++) {
				s += i;
			}
			safepointLeaveBlocked(&me);
		}
	}
	safepointUnregister(&me);

	__atomic_fetch_add(&gWorkDone, work, __ATOMIC_RELAXED);
	__atomic_fetch_add(&gRaces, races, __ATOMIC_RELAXED);
	return NULL;
}


int safepointSelfTest(void)
{
	safepointInit();
	gInSafepoint = 0;
	gRaces = 0;
	gWorkDone = 0;
	__atomic_store_n(&gStopWorkers, 0, __ATOMIC_RELEASE);

	pthread_t workers[SP_TEST_WORKERS];
	for (long i = 0; i < SP_TEST_WORKERS; i++) {
		pthread_create(&workers[i], NULL, safepointTestWorker, (void *) i);
	}

	for (int r = 0; r < SP_TEST_STOPS; r++) {
		safepointBegin(NULL); // coordinator is not registered -> exclude nothing
		// Exclusive region: no worker may run here.
		gInSafepoint = 1;
		volatile long y = 0;
		for (int i = 0; i < 200; i++) {
			y += i;
		}
		gInSafepoint = 0; // clear before releasing so no worker can ever see 1
		safepointEnd();
	}

	__atomic_store_n(&gStopWorkers, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < SP_TEST_WORKERS; i++) {
		pthread_join(workers[i], NULL);
	}

	fprintf(stderr, "safepoint self-test: stops=%d workers=%d workDone=%ld races=%ld -> %s\n",
		SP_TEST_STOPS, SP_TEST_WORKERS, gWorkDone, gRaces,
		(gRaces == 0 && gWorkDone > 0) ? "PASS" : "FAIL");

	return (gRaces == 0 && gWorkDone > 0) ? 0 : 1;
}
