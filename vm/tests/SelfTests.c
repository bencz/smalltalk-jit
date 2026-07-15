// The C-level multicore/concurrency self-test battery, dispatched at runtime
// by ST_*_TEST environment variables (see selfTestFromEnv at the bottom).
// Extracted verbatim from main.c; each test is a falsifiable harness for one
// VM mechanism (safepoints, TLAB, parallel GC, the multi-worker scheduler...).
// Always run these SANDBOXED (systemd-run + taskset) — see the project notes.
#include "vm/tests/SelfTests.h"
#include "vm/core/Entry.h"
#include "vm/tools/Snapshot.h"
#include "vm/core/Thread.h"
#include "vm/concurrency/Scheduler.h"
#include "vm/runtime/Message.h"
#include "vm/memory/Safepoint.h"
#include "vm/core/Handle.h"
#include "vm/core/Smalltalk.h"
#include "vm/memory/Heap.h"
#include "vm/core/Exception.h"
#include "vm/os/Os.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>


static void runMessageTest(void *arg)
{
	*(int *) arg = messageSelfTest();
}


// ---- worker-thread Smalltalk execution self-test (ST_WORKER_TEST=1) --------
// Proves a SECOND OS thread can run JIT-compiled Smalltalk on the SAME heap:
// the worker points its Thread at the shared heap, replicates the well-known
// Handles (they reference shared objects), and runs a block that allocates
// heavily (forcing scavenges on the worker) — all sharing one heap, one set of
// compiled methods and JIT stubs (which reach per-thread state via CTX->thread).
static SmalltalkHandles gMainHandles;
static Heap *gWorkerHeap;
// The block to run, held as a PERSISTENT HANDLE on main (not a raw Value global):
// the block promotes/moves during warm-up, and only a handle's `raw` is kept current
// by the GC. Workers read `gWorkerBlockH->raw` (fresh) — a raw global would go stale
// and point a worker at a reclaimed location.
static Object *gWorkerBlockH;
static Value gWorkerResult;

static void *workerRunBlock(void *arg)
{
	(void) arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = gWorkerHeap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(gWorkerHeap, &CurrentThread);
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler; // this worker's own TLS slot
	// Handles are per-heap now (Handle.h): CurrentThread.heap already points at the
	// shared heap, whose handles are populated — no TLS copy needed.
	initThreadContext(&CurrentThread);               // root context on the worker's TLAB
	HandleScope scope;
	openHandleScope(&scope);                         // entry points below re-handle into a parent
	Object *blockH = handle(gWorkerBlockH->raw);     // fresh block from main's live handle
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, blockH);                // handle arg (GC-updated across stub gen)
	gWorkerResult = sendMessage(getSymbol("value"), &args);
	closeHandleScope(&scope, NULL);
	heapEndMutator(gWorkerHeap, &CurrentThread); // leave heap->mutators before this thread dies
	return NULL;
}

static int workerSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;
	// A clean, allocation-heavy block: 200k Array allocations => many scavenges on
	// the worker; returns 2*200000 = 400000.
	gWorkerBlockH = handle(asObject(evalObject("[ | n | n := 0. 1 to: 200000 do: [:i | n := n + (Array new: 2) size]. n ]")));

	pthread_t worker;
	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main is idle: safe for the worker's GC
	pthread_create(&worker, NULL, workerRunBlock, NULL);
	pthread_join(worker, NULL);
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	closeHandleScope(&scope, NULL);
	long result = valueTypeOf(gWorkerResult, VALUE_INT) ? (long) asCInt(gWorkerResult) : -1;
	fprintf(stderr, "worker self-test: a Smalltalk block ran on a WORKER OS thread sharing the heap -> result=%ld (expected 400000) -> %s\n",
		result, result == 400000 ? "PASS" : "FAIL");
	return result == 400000 ? 0 : 1;
}


// ---- parallel Smalltalk execution self-test (ST_PARALLEL_TEST=1) -----------
// N worker OS threads run the SAME compiled block CONCURRENTLY on the shared
// heap. The block is pure integer compute (no allocation => no GC => no need for
// the safepoint-coordinated collector yet), so this isolates and proves REAL
// parallel Smalltalk execution across cores. Reports the speedup vs one thread.
#define PAR_WORKERS 8
static Value gParResults[PAR_WORKERS];

static void *parWorker(void *arg)
{
	long id = (long) arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = gWorkerHeap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(gWorkerHeap, &CurrentThread);
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler; // this worker's own TLS slot
	// Handles are per-heap now (shared via CurrentThread.heap) — no TLS copy needed.
	initThreadContext(&CurrentThread);
	HandleScope scope;
	openHandleScope(&scope);
	Object *blockH = handle(gWorkerBlockH->raw); // fresh block from main's live handle
	EntryArgs args = { .size = 0 };
	// Pass the receiver as a HANDLE (GC-updated), never a raw value: sendMessage can
	// allocate (per-thread stub gen) before it reads args[0], so a raw value could be
	// left stale by a peer-pressured scavenge.
	entryArgsAddObject(&args, blockH);
	gParResults[id] = sendMessage(getSymbol("value"), &args);
	closeHandleScope(&scope, NULL);
	heapEndMutator(gWorkerHeap, &CurrentThread); // leave heap->mutators before this thread dies
	return NULL;
}

static Value runBlockOnce(Value block)
{
	HandleScope scope;
	openHandleScope(&scope);
	Object *h = handle(asObject(block));
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, h); // handle arg (GC-updated across sendMessage's stub gen)
	Value r = sendMessage(getSymbol("value"), &args);
	closeHandleScope(&scope, NULL);
	return r;
}

static int parallelSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;
	// Pure-compute block: sum 1..30,000,000 (fits SmallInteger, so NO allocation).
	gWorkerBlockH = handle(asObject(evalObject("[ | s | s := 0. 1 to: 30000000 do: [:i | s := s + i]. s ]")));
	long expected = 450000015000000L; // 30000000 * 30000001 / 2

	runBlockOnce(getTaggedPtr(gWorkerBlockH)); // warm up JIT compilation so workers only READ cached code
	int64_t s0 = osCurrentMicroTime();
	runBlockOnce(getTaggedPtr(gWorkerBlockH));  // serial baseline: one run
	int64_t serialUs = osCurrentMicroTime() - s0;

	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main idle while the workers run
	int64_t p0 = osCurrentMicroTime();
	pthread_t th[PAR_WORKERS];
	for (long w = 0; w < PAR_WORKERS; w++) {
		pthread_create(&th[w], NULL, parWorker, (void *) w);
	}
	for (int w = 0; w < PAR_WORKERS; w++) {
		pthread_join(th[w], NULL);
	}
	int64_t parUs = osCurrentMicroTime() - p0;
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	int ok = 1;
	for (int w = 0; w < PAR_WORKERS; w++) {
		long r = valueTypeOf(gParResults[w], VALUE_INT) ? (long) asCInt(gParResults[w]) : -1;
		if (r != expected) {
			ok = 0;
		}
	}
	closeHandleScope(&scope, NULL);

	double speedup = parUs > 0 ? (double) PAR_WORKERS * (double) serialUs / (double) parUs : 0.0;
	fprintf(stderr, "parallel self-test: %d workers ran Smalltalk sum(1..30M) CONCURRENTLY on the shared heap | serial=%ldms parallel(%dx work)=%ldms speedup=%.1fx | all-correct=%d -> %s\n",
		PAR_WORKERS, (long) (serialUs / 1000), PAR_WORKERS, (long) (parUs / 1000), speedup, ok, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}


// ---- parallel Smalltalk WITH allocation + safepoint-coordinated GC ---------
// N workers each run an allocation-heavy block CONCURRENTLY: 400k short-lived
// Arrays => many young scavenges. When one worker's nursery fills it becomes the
// collector (heapGcBegin parks the others at their allocation polls), scavenges
// the shared heap scanning EVERY mutator's roots, then resumes them. If the
// safepoint/root-scan were wrong, a worker's live objects would be moved out from
// under it and the result would be corrupt.
static int parallelGcSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;
	gWorkerBlockH = handle(asObject(evalObject("[ | n | n := 0. 1 to: 400000 do: [:i | n := n + (Array new: 2) size]. n ]")));
	long expected = 800000; // 2 * 400000
	runBlockOnce(getTaggedPtr(gWorkerBlockH)); // warm up JIT compilation

	heapGcEnterBlocked(gWorkerHeap, &CurrentThread);
	int64_t t0 = osCurrentMicroTime();
	pthread_t th[PAR_WORKERS];
	for (long w = 0; w < PAR_WORKERS; w++) {
		pthread_create(&th[w], NULL, parWorker, (void *) w);
	}
	for (int w = 0; w < PAR_WORKERS; w++) {
		pthread_join(th[w], NULL);
	}
	int64_t us = osCurrentMicroTime() - t0;
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	int ok = 1;
	for (int w = 0; w < PAR_WORKERS; w++) {
		long r = valueTypeOf(gParResults[w], VALUE_INT) ? (long) asCInt(gParResults[w]) : -1;
		if (r != expected) {
			ok = 0;
		}
	}
	closeHandleScope(&scope, NULL);
	fprintf(stderr, "parallel-GC self-test: %d workers each allocated 400k Arrays CONCURRENTLY on the shared heap in %ldms (safepoint-coordinated scavenges) | all-correct=%d -> %s\n",
		PAR_WORKERS, (long) (us / 1000), ok, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}


// ---- parallel Smalltalk WITH promotion + multi-thread FULL GC --------------
// N workers each build a LARGE, long-lived structure (an OrderedCollection of
// 100k two-element Arrays) held in a live local, then re-read every element to a
// checksum. The structures survive scavenges and promote to old space; across 8
// workers that pushes old space past its full-GC threshold, so a scavenge with a
// promotion failure escalates to markAndSweep — under a stop-the-world safepoint
// while the OTHER workers sit parked at an allocation poll with their partial
// structures live on their stacks. The full GC therefore MUST scan every
// mutator's roots (not just the collector's): if it didn't, a peer's live Arrays
// would be swept, its backing memory recycled, and its checksum would come out
// wrong (or it would crash dereferencing a reclaimed object). gFullGcRuns proves
// the multi-thread full-GC path actually fired.
extern unsigned long gFullGcRuns;

static int parallelFullGcSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;
	// Build then verify: acc stays live the whole time (a root on the worker's
	// stack), so its 100k Arrays must survive every peer's full GC.
	gWorkerBlockH = handle(asObject(evalObject(
		"[ | acc sum | "
		"  acc := OrderedCollection new. "
		"  1 to: 100000 do: [:i | acc add: (Array with: i with: i * 2) ]. "
		"  sum := 0. "
		"  acc do: [:a | sum := sum + (a at: 1) + (a at: 2) ]. "
		"  sum ]")));
	long expected = 15000150000L; // sum over i=1..100000 of (i + 2i) = 3 * 100000*100001/2
	unsigned long fullGcBefore = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED);
	runBlockOnce(getTaggedPtr(gWorkerBlockH)); // warm up JIT compilation so workers only READ cached code

	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main idle while the workers run
	int64_t t0 = osCurrentMicroTime();
	pthread_t th[PAR_WORKERS];
	for (long w = 0; w < PAR_WORKERS; w++) {
		pthread_create(&th[w], NULL, parWorker, (void *) w);
	}
	for (int w = 0; w < PAR_WORKERS; w++) {
		pthread_join(th[w], NULL);
	}
	int64_t us = osCurrentMicroTime() - t0;
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	int ok = 1;
	for (int w = 0; w < PAR_WORKERS; w++) {
		long r = valueTypeOf(gParResults[w], VALUE_INT) ? (long) asCInt(gParResults[w]) : -1;
		if (r != expected) {
			ok = 0;
		}
	}
	unsigned long fullGcs = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED) - fullGcBefore;
	closeHandleScope(&scope, NULL);
	fprintf(stderr, "parallel-full-GC self-test: %d workers each built+verified a 100k-Array structure CONCURRENTLY on the shared heap in %ldms | full GCs (markAndSweep) observed=%lu | all-correct=%d -> %s\n",
		PAR_WORKERS, (long) (us / 1000), fullGcs, ok, (ok && fullGcs > 0) ? "PASS" : "FAIL");
	return (ok && fullGcs > 0) ? 0 : 1;
}


// ---- scheduler-active collector must scan EVERY mutator (B0) ---------------
// Forces the scenario the pre-B0 GC got wrong: a collector whose OWN scheduler
// is active (gActive==1) used to scan only ITS OWN fibers and skip heap->mutators
// entirely (Scavenger.c schedulerActive() branch / GarbageCollector.c early
// returns). Here the MAIN thread runs its scheduler and an allocation-heavy fiber
// (so main becomes the multi-mutator collector WITH gActive==1), while N peer OS
// threads each hold a YOUNG structure reachable ONLY through their own handle
// scope — precisely the root the buggy branch skips for peers. If the collector
// skips them, the peers' structures are stranded in the abandoned semispace, its
// memory reused by later evacuation, and the peers' checksums come out wrong (or
// they crash dereferencing reclaimed objects). Pre-B0: FAIL/crash. Post-B0: PASS.
#define SCHEDGC_PEERS 6
extern unsigned long gFullGcRuns;
static Object *gSchedBuildBlockH;
static Object *gSchedVerifyBlockH;
static Object *gSchedCollectorBlockH;
static int gSchedResults[SCHEDGC_PEERS];
static int gSchedReady = 0;
static int gSchedDone = 0;
static long gSchedExpected;

static void *schedGcPeer(void *arg)
{
	long id = (long) arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = gWorkerHeap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(gWorkerHeap, &CurrentThread);
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler; // this peer's own TLS slot
	// Handles are per-heap now (shared via CurrentThread.heap) — no TLS copy needed.
	initThreadContext(&CurrentThread);
	HandleScope scope;
	openHandleScope(&scope);
	Object *buildH = handle(gSchedBuildBlockH->raw); // persistent handle to the (old) block
	EntryArgs bargs = { .size = 0 };
	entryArgsAddObject(&bargs, buildH);
	Value structure = sendMessage(getSymbol("value"), &bargs);
	// Hold the YOUNG structure ONLY in this handle SCOPE (thread->handleScopes) —
	// the exact root a gActive==1 collector skips for peer mutators. A persistent
	// handle() would be scanned by iteratePersistentHandles regardless of the branch
	// and could not discriminate the bug; no old->young edge either.
	Object *structH = scopeHandle(asObject(structure));

	__atomic_add_fetch(&gSchedReady, 1, __ATOMIC_RELEASE);
	// Park (counts as safe for the collector) until main has finished collecting.
	heapGcEnterBlocked(gWorkerHeap, &CurrentThread);
	while (!__atomic_load_n(&gSchedDone, __ATOMIC_ACQUIRE)) {
		usleep(200);
	}
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	// Re-read the structure via its (possibly-forwarded) handle and re-checksum.
	Object *verifyH = handle(gSchedVerifyBlockH->raw);
	EntryArgs vargs = { .size = 0 };
	entryArgsAddObject(&vargs, verifyH);  // receiver: the [:a| ...] block
	entryArgsAddObject(&vargs, structH);  // arg: the structure
	Value sum = sendMessage(getSymbol("value:"), &vargs);
	long got = valueTypeOf(sum, VALUE_INT) ? (long) asCInt(sum) : -1;
	gSchedResults[id] = (got == gSchedExpected) ? 1 : 0;

	closeHandleScope(&scope, NULL);
	heapEndMutator(gWorkerHeap, &CurrentThread);
	return NULL;
}

static void schedGcCollectorFiber(void *arg)
{
	(void) arg;
	// Runs as fiber #0 under main's scheduler (gActive==1, gCurrent!=NULL). Its
	// allocation-heavy block holds every survivor live, so the shared young space
	// cycles through several scavenges that evacuate INTO the abandoned semispace
	// where the parked peers' stranded structures sit — guaranteeing reuse.
	runBlockOnce(getTaggedPtr(gSchedCollectorBlockH));
}

static int schedGcSelfTest(void)
{
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;
	gSchedExpected = 1501500; // sum over i=1..1000 of (i + 2*i) = 3*1000*1001/2

	// Compile + warm up the blocks on MAIN while gActive==0 (correct single-mutator
	// scanning), so peers and the collector fiber only READ cached native code and
	// never compile/allocate under the buggy branch during the concurrent phase. The
	// block handles are PERSISTENT (thread->handles), so they survive without holding
	// a HandleScope across schedulerRun (which swaps thread->handleScopes per fiber).
	{
		HandleScope warm;
		openHandleScope(&warm);
		gSchedBuildBlockH = handle(asObject(evalObject(
			"[ | a | a := Array new: 1000. 1 to: 1000 do: [:i | a at: i put: (Array with: i with: i * 2)]. a ]")));
		gSchedVerifyBlockH = handle(asObject(evalObject(
			"[:a | | s | s := 0. a do: [:e | s := s + (e at: 1) + (e at: 2)]. s ]")));
		gSchedCollectorBlockH = handle(asObject(evalObject(
			"[ | acc | acc := OrderedCollection new. 1 to: 500000 do: [:i | acc add: (Array with: i with: i * 2)]. acc size ]")));
		// warm-up + sanity check of the expected checksum
		Value s = runBlockOnce(getTaggedPtr(gSchedBuildBlockH));
		Object *sH = scopeHandle(asObject(s));
		Object *vH = handle(gSchedVerifyBlockH->raw);
		EntryArgs wargs = { .size = 0 };
		entryArgsAddObject(&wargs, vH);
		entryArgsAddObject(&wargs, sH);
		Value w = sendMessage(getSymbol("value:"), &wargs);
		if (!valueTypeOf(w, VALUE_INT) || (long) asCInt(w) != gSchedExpected) {
			fprintf(stderr, "sched-active-GC self-test: warm-up checksum mismatch -> FAIL\n");
			closeHandleScope(&warm, NULL);
			return 1;
		}
		runBlockOnce(getTaggedPtr(gSchedCollectorBlockH)); // warm up JIT of the collector block
		closeHandleScope(&warm, NULL);
	}

	// Activate main's scheduler: from here main is gActive==1.
	schedulerInit();

	// Force the full-GC path too: main's scavenges promote the peers' held structures
	// to OLD space, and a low threshold makes the collector fiber's heavy allocation
	// trip markAndSweep — which (with gActive==1) must ALSO mark every peer's roots or
	// their promoted structures get swept. Covers both collectors in one run.
	gWorkerHeap->oldGcThreshold = 1 * 1024 * 1024;
	unsigned long fullGcBefore = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED);

	__atomic_store_n(&gSchedReady, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&gSchedDone, 0, __ATOMIC_RELEASE);

	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main safe while peers build+park
	pthread_t th[SCHEDGC_PEERS];
	for (long w = 0; w < SCHEDGC_PEERS; w++) {
		pthread_create(&th[w], NULL, schedGcPeer, (void *) w);
	}
	while (__atomic_load_n(&gSchedReady, __ATOMIC_ACQUIRE) < SCHEDGC_PEERS) {
		usleep(200);
	}
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	// main becomes the multi-mutator collector WITH gActive==1: run the allocating
	// fiber under the scheduler so its scavenges take the schedulerActive() branch.
	schedulerSpawnC(schedGcCollectorFiber, NULL, 0);
	schedulerRun();

	__atomic_store_n(&gSchedDone, 1, __ATOMIC_RELEASE);
	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main safe while peers verify+exit
	for (int w = 0; w < SCHEDGC_PEERS; w++) {
		pthread_join(th[w], NULL);
	}
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	int ok = 1;
	for (int w = 0; w < SCHEDGC_PEERS; w++) {
		if (!gSchedResults[w]) {
			ok = 0;
		}
	}
	unsigned long fullGcs = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED) - fullGcBefore;
	fprintf(stderr, "sched-active-GC self-test: main ran a scheduler+alloc fiber (gActive==1) while %d peers held structures parked | scavenges+full GCs (markAndSweep)=%lu | all-survived=%d -> %s\n",
		SCHEDGC_PEERS, fullGcs, ok, (ok && fullGcs > 0) ? "PASS" : "FAIL");
	return (ok && fullGcs > 0) ? 0 : 1;
}


// ---- JIT back-edge safepoint poll: CPU-bound loops must yield to a peer GC (B1) ----
// N peer OS threads each run a PURE, NON-allocating Smalltalk loop (count to 8e8,
// SmallInteger => no allocation). The only way a peer running this reaches a
// safepoint is the new JIT back-edge poll — it never hits the allocate() poll.
// While they loop, main (a registered mutator) requests several stop-the-world
// rounds via heapGcBegin. WITHOUT the poll a peer never becomes safe, so
// heapGcBegin blocks until the peer FINISHES its ~2-3s loop and deregisters ->
// each round takes ~loop-time. WITH the poll, peers park within one iteration ->
// each round returns in microseconds. The pre/post gap is ~4 orders of magnitude,
// so a 200ms threshold makes the pass/fail deterministic and non-flaky.
#define SPJIT_PEERS 4
static int gSpjitReady = 0;
static int gSpjitResults[SPJIT_PEERS];
static Value gSpjitExpected;

static void *spjitPeer(void *arg)
{
	long id = (long) arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = gWorkerHeap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(gWorkerHeap, &CurrentThread);
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler;
	// Handles are per-heap now (shared via CurrentThread.heap) — no TLS copy needed.
	initThreadContext(&CurrentThread);
	HandleScope scope;
	openHandleScope(&scope);
	Object *blockH = handle(gWorkerBlockH->raw);
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, blockH);
	__atomic_add_fetch(&gSpjitReady, 1, __ATOMIC_RELEASE); // ready just before the pure loop
	Value r = sendMessage(getSymbol("value"), &args);      // long non-allocating loop
	gSpjitResults[id] = (valueTypeOf(r, VALUE_INT) && asCInt(r) == asCInt(gSpjitExpected)) ? 1 : 0;
	closeHandleScope(&scope, NULL);
	heapEndMutator(gWorkerHeap, &CurrentThread);
	return NULL;
}

static int safepointJitSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;
	// Pure-compute, NON-allocating loop on the JIT's inlined to:do: machine loop:
	// s counts to 8e8 (fits SmallInteger, so no LargeInteger allocation). Long
	// enough (~2-3s) that a pre-fix blocked heapGcBegin dwarfs the threshold.
	gWorkerBlockH = handle(asObject(evalObject("[ | s | s := 0. 1 to: 800000000 do: [:i | s := s + 1]. s ]")));
	gSpjitExpected = tagInt(800000000L);
	runBlockOnce(getTaggedPtr(gWorkerBlockH)); // warm up JIT so peers only READ cached code

	__atomic_store_n(&gSpjitReady, 0, __ATOMIC_RELEASE);
	pthread_t th[SPJIT_PEERS];
	for (long w = 0; w < SPJIT_PEERS; w++) {
		pthread_create(&th[w], NULL, spjitPeer, (void *) w);
	}
	while (__atomic_load_n(&gSpjitReady, __ATOMIC_ACQUIRE) < SPJIT_PEERS) {
		usleep(200);
	}
	usleep(50 * 1000); // settle: let peers pass sendMessage's entry allocation (which hits
	                   // the allocate() poll) and get into the pure steady loop.

	const int rounds = 4;
	int64_t worstUs = 0;
	for (int r = 0; r < rounds; r++) {
		int64_t t0 = osCurrentMicroTime();
		heapGcBegin(gWorkerHeap, &CurrentThread); // requires every peer at a safepoint
		int64_t us = osCurrentMicroTime() - t0;
		heapGcEnd(gWorkerHeap);
		if (us > worstUs) {
			worstUs = us;
		}
	}

	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main safe while peers finish + exit
	for (int w = 0; w < SPJIT_PEERS; w++) {
		pthread_join(th[w], NULL);
	}
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	int ok = 1;
	for (int w = 0; w < SPJIT_PEERS; w++) {
		if (!gSpjitResults[w]) {
			ok = 0;
		}
	}
	closeHandleScope(&scope, NULL);
	int64_t thresholdUs = 200 * 1000; // pre-fix a blocked heapGcBegin waits ~loop-time (>1s)
	int fast = worstUs < thresholdUs;
	fprintf(stderr, "safepoint-JIT self-test: %d peers ran a NON-allocating loop; %d STW rounds mid-loop, worst heapGcBegin=%ldms (threshold %ldms) | all-correct=%d -> %s\n",
		SPJIT_PEERS, rounds, (long) (worstUs / 1000), (long) (thresholdUs / 1000), ok, (ok && fast) ? "PASS" : "FAIL");
	return (ok && fast) ? 0 : 1;
}


// ---- JIT back-edge stackmap correctness: loop-carried body-only root (B1 Q4) ----
// The linear-scan allocator's [start,end] liveness is control-flow-unaware, so a
// young pointer that is loop-carried but whose last TEXTUAL use precedes the
// back-edge is omitted from the poll's stackmap unless we over-approximate. Here
// each peer builds a young Array `box`, anchors it in a shared array slot (a
// SECOND, independently-scanned root) and then runs a NON-allocating loop that
// writes into box WITHOUT referencing it after the loop (so box's frame slot is
// body-only). Main drives a real scavenge while every peer is parked at the
// back-edge poll: it relocates box (found via the shared slot) and, WITH the fix,
// also updates the peer's frame slot so the peer keeps writing into the relocated
// box; WITHOUT the fix the frame slot is stale, the peer's post-scavenge writes go
// to the abandoned copy, and the shared slot's box freezes at the scavenge-time
// value != BIG. Pre-fix: FAIL (or crash). Post-fix: PASS.
#define SPROOTS_PEERS 4
#define SPROOTS_BIG 200000000L
static int gRootsReady = 0;
static Object *gRootsPeerBlockH;
static Object *gRootsReadBlockH;
static Object *gRootsSharedH;

static void *rootsPeer(void *arg)
{
	long id = (long) arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = gWorkerHeap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(gWorkerHeap, &CurrentThread);
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler;
	// Handles are per-heap now (shared via CurrentThread.heap) — no TLS copy needed.
	initThreadContext(&CurrentThread);
	HandleScope scope;
	openHandleScope(&scope);
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, handle(gRootsPeerBlockH->raw)); // receiver: [:shared :idx | ...]
	entryArgsAddObject(&args, handle(gRootsSharedH->raw));    // arg1: shared array
	entryArgsAdd(&args, tagInt(id + 1));                      // arg2: 1-based slot index
	__atomic_add_fetch(&gRootsReady, 1, __ATOMIC_RELEASE);   // ready just before the write-loop
	sendMessage(getSymbol("value:value:"), &args);
	closeHandleScope(&scope, NULL);
	heapEndMutator(gWorkerHeap, &CurrentThread);
	return NULL;
}

static int safepointRootsSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	gWorkerHeap = CurrentThread.heap;
	gMainHandles = Handles;

	// box is loop-carried but body-only: written inside the loop, NOT referenced
	// after it (returns nil), so its frame slot is omitted from the back-edge
	// stackmap unless generateSafepointPoll over-approximates. The loop bound is a
	// LITERAL: only then does the compiler emit the inlined to:do: machine loop with
	// a back-edge (a variable bound degenerates into real sends with no back-edge).
	char blockSrc[256];
	snprintf(blockSrc, sizeof(blockSrc),
		"[:shared :idx | | box | box := Array new: 1. box at: 1 put: 0. "
		"shared at: idx put: box. 1 to: %ld do: [:k | box at: 1 put: k]. nil ]", SPROOTS_BIG);
	gRootsPeerBlockH = handle(asObject(evalObject(blockSrc)));
	gRootsReadBlockH = handle(asObject(evalObject("[:shared :idx | (shared at: idx) at: 1 ]")));
	char arrSrc[64];
	snprintf(arrSrc, sizeof(arrSrc), "Array new: %d", SPROOTS_PEERS);
	gRootsSharedH = handle(asObject(evalObject(arrSrc)));

	// Warm up (compile) the peer block on MAIN with a full run, so peers only READ
	// cached native code + inline caches during the concurrent phase and never
	// compile/allocate/lookup (which would let them reach a safepoint without the
	// back-edge poll). It writes a throwaway box into shared[1]; peers overwrite it.
	{
		EntryArgs wargs = { .size = 0 };
		entryArgsAddObject(&wargs, handle(gRootsPeerBlockH->raw));
		entryArgsAddObject(&wargs, handle(gRootsSharedH->raw));
		entryArgsAdd(&wargs, tagInt(1));
		sendMessage(getSymbol("value:value:"), &wargs);
	}

	__atomic_store_n(&gRootsReady, 0, __ATOMIC_RELEASE);
	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main safe while peers build + start looping
	pthread_t th[SPROOTS_PEERS];
	for (long w = 0; w < SPROOTS_PEERS; w++) {
		pthread_create(&th[w], NULL, rootsPeer, (void *) w);
	}
	while (__atomic_load_n(&gRootsReady, __ATOMIC_ACQUIRE) < SPROOTS_PEERS) {
		usleep(200);
	}
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);
	usleep(50 * 1000); // settle: peers past the setup allocation and into the pure write-loop

	// Drive a REAL stop-the-world scavenge while peers are parked at the back-edge
	// poll (mirrors heapCollectYoung's multi-mutator branch). It relocates each
	// peer's young box; the frame slot must move with it (the Q4 fix) or later
	// writes are lost.
	heapGcEnterBlocked(gWorkerHeap, &CurrentThread);
	pthread_mutex_lock(&gWorkerHeap->gcLock);
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);
	heapGcBegin(gWorkerHeap, &CurrentThread);      // park every peer at its back-edge poll
	scavengerScavenge(&gWorkerHeap->newSpace);     // move box; update shared[idx] + (with fix) frame slot
	heapGcEnd(gWorkerHeap);
	pthread_mutex_unlock(&gWorkerHeap->gcLock);

	heapGcEnterBlocked(gWorkerHeap, &CurrentThread); // main safe while peers run to BIG + exit
	for (int w = 0; w < SPROOTS_PEERS; w++) {
		pthread_join(th[w], NULL);
	}
	heapGcLeaveBlocked(gWorkerHeap, &CurrentThread);

	// Verify: shared[idx] at:1 must equal BIG (the last k written). With the bug the
	// post-scavenge writes went to a stale slot and this is frozen earlier.
	int ok = 1;
	for (long w = 0; w < SPROOTS_PEERS; w++) {
		EntryArgs rargs = { .size = 0 };
		entryArgsAddObject(&rargs, handle(gRootsReadBlockH->raw));
		entryArgsAddObject(&rargs, handle(gRootsSharedH->raw));
		entryArgsAdd(&rargs, tagInt(w + 1));
		Value v = sendMessage(getSymbol("value:value:"), &rargs);
		long got = valueTypeOf(v, VALUE_INT) ? (long) asCInt(v) : -1;
		if (got != SPROOTS_BIG) {
			ok = 0;
		}
	}
	closeHandleScope(&scope, NULL);
	fprintf(stderr, "safepoint-roots self-test: %d peers held a loop-carried body-only young Array across a NON-allocating loop; a scavenge relocated it while they were parked at the back-edge poll | all-slots-tracked=%d -> %s\n",
		SPROOTS_PEERS, ok, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}


// ---- B2.3: multi-worker scheduler pool self-tests --------------------------
// These drive the REAL scheduler pool (M workers over one shared heap via
// schedulerRun with ST_SCHED_WORKERS>1) with fibers that coordinate ONLY through
// scheduler primitives (yield / sleep / the pool itself), never Smalltalk sync
// (which is not thread-safe until B3). A watchdog turns a hang into a hard FAIL.

static _Atomic int gWatchdogDone;
static void *schedWatchdog(void *arg)
{
	long ms = (long) arg;
	long waited = 0;
	while (!__atomic_load_n(&gWatchdogDone, __ATOMIC_ACQUIRE) && waited < ms) {
		usleep(10000);
		waited += 10;
	}
	if (!__atomic_load_n(&gWatchdogDone, __ATOMIC_ACQUIRE)) {
		fprintf(stderr, "WATCHDOG: scheduler pool hung (lost wakeup / deadlock) -> FAIL\n");
		_exit(99);
	}
	return NULL;
}

static int schedWorkerCount(void)
{
	char *w = getenv("ST_SCHED_WORKERS");
	return w ? atoi(w) : 1;
}

// 1. ST_SCHED_PARALLEL_TEST — N CPU-bound fibers distributed across the pool. Pure C
//    compute (no allocation, no yield): each pool worker runs one fiber to completion,
//    so wall time drops ~1/workers. Correctness proves the pool runs every fiber.
#define SCP_FIBERS 8
static _Atomic long gScpResult[SCP_FIBERS];
static void scpFiber(void *arg)
{
	long id = (long) arg;
	volatile long sum = 0; // volatile so the optimizer can't fold the loop to a closed form
	for (long i = 1; i <= 30000000L; i++) {
		sum += i;
	}
	__atomic_store_n(&gScpResult[id], sum, __ATOMIC_RELAXED);
}

static int schedParallelSelfTest(void)
{
	long expected = 30000000L * 30000001L / 2; // 450000015000000
	int workers = schedWorkerCount();
	schedulerInit();
	for (long i = 0; i < SCP_FIBERS; i++) {
		schedulerSpawnC(scpFiber, (void *) i, 0);
	}
	int64_t t0 = osCurrentMicroTime();
	schedulerRun();
	int64_t us = osCurrentMicroTime() - t0;
	int ok = 1;
	for (int i = 0; i < SCP_FIBERS; i++) {
		if (__atomic_load_n(&gScpResult[i], __ATOMIC_RELAXED) != expected) {
			ok = 0;
		}
	}
	fprintf(stderr, "sched-parallel self-test: %d CPU-bound fibers ran across %d pool worker(s) in %ldms | all-correct=%d -> %s\n",
		SCP_FIBERS, workers, (long) (us / 1000), ok, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

// 2. ST_SCHED_DOUBLERUN_TEST — no fiber ever runs on two workers at once (M2 handoff).
//    Each fiber claims a per-fiber flag (atomic exchange must see 0), does tiny work,
//    releases, then yields — hammering the pop/commit-after-switch/re-pop handoff.
#define SDR_FIBERS 12
#define SDR_ROUNDS 1500
static _Atomic int gSdrRunning[SDR_FIBERS];
static _Atomic long gSdrRounds[SDR_FIBERS];
static _Atomic int gSdrDouble;
static void sdrFiber(void *arg)
{
	long id = (long) arg;
	for (int r = 0; r < SDR_ROUNDS; r++) {
		if (__atomic_exchange_n(&gSdrRunning[id], 1, __ATOMIC_ACQ_REL) != 0) {
			__atomic_store_n(&gSdrDouble, 1, __ATOMIC_RELAXED); // same stack on two workers!
		}
		for (volatile int k = 0; k < 40; k++) { }
		__atomic_store_n(&gSdrRunning[id], 0, __ATOMIC_RELEASE);
		__atomic_add_fetch(&gSdrRounds[id], 1, __ATOMIC_RELAXED);
		schedulerYield();
	}
}

static int schedDoubleRunSelfTest(void)
{
	int workers = schedWorkerCount();
	schedulerInit();
	for (long i = 0; i < SDR_FIBERS; i++) {
		schedulerSpawnC(sdrFiber, (void *) i, 0);
	}
	__atomic_store_n(&gWatchdogDone, 0, __ATOMIC_RELEASE);
	pthread_t wd;
	pthread_create(&wd, NULL, schedWatchdog, (void *) 120000L);
	schedulerRun();
	__atomic_store_n(&gWatchdogDone, 1, __ATOMIC_RELEASE);
	pthread_join(wd, NULL);
	long total = 0;
	for (int i = 0; i < SDR_FIBERS; i++) {
		total += __atomic_load_n(&gSdrRounds[i], __ATOMIC_RELAXED);
	}
	int dbl = __atomic_load_n(&gSdrDouble, __ATOMIC_RELAXED);
	int ok = !dbl && (total == (long) SDR_FIBERS * SDR_ROUNDS);
	fprintf(stderr, "sched-doublerun self-test: %d fibers x %d yield-rounds on %d workers | double-run=%d total=%ld/%d -> %s\n",
		SDR_FIBERS, SDR_ROUNDS, workers, dbl, total, SDR_FIBERS * SDR_ROUNDS, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

// 3. ST_SCHED_LOSTWAKE_TEST — no wakeup is ever lost (M2 parkPending). Each fiber does
//    R rounds of schedulerSleep(0): it parks on a timer while a poller (another worker)
//    fires that timer and resumes it, maximizing the resume-races-PARKING interleaving.
//    Without parkPending a raced resume is dropped -> that fiber never wakes -> the
//    total falls short (or the pool hangs -> watchdog).
#define SLW_FIBERS 8
#define SLW_ROUNDS 1000
static _Atomic long gSlwRounds[SLW_FIBERS];
static void slwFiber(void *arg)
{
	long id = (long) arg;
	for (int r = 0; r < SLW_ROUNDS; r++) {
		schedulerSleep(0); // park on timer; the poller resumes us (parkPending stress)
		__atomic_add_fetch(&gSlwRounds[id], 1, __ATOMIC_RELAXED);
	}
}

static int schedLostWakeSelfTest(void)
{
	int workers = schedWorkerCount();
	schedulerInit();
	for (long i = 0; i < SLW_FIBERS; i++) {
		schedulerSpawnC(slwFiber, (void *) i, 0);
	}
	__atomic_store_n(&gWatchdogDone, 0, __ATOMIC_RELEASE);
	pthread_t wd;
	pthread_create(&wd, NULL, schedWatchdog, (void *) 120000L);
	schedulerRun();
	__atomic_store_n(&gWatchdogDone, 1, __ATOMIC_RELEASE);
	pthread_join(wd, NULL);
	long total = 0;
	for (int i = 0; i < SLW_FIBERS; i++) {
		total += __atomic_load_n(&gSlwRounds[i], __ATOMIC_RELAXED);
	}
	int ok = (total == (long) SLW_FIBERS * SLW_ROUNDS);
	fprintf(stderr, "sched-lostwake self-test: %d fibers x %d sleep(0) rounds on %d workers (timer/park race) | total=%ld/%d -> %s\n",
		SLW_FIBERS, SLW_ROUNDS, workers, total, SLW_FIBERS * SLW_ROUNDS, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

// 4. ST_SCHED_STW_TEST — idle pool workers are GC-safe (M3). A couple of fibers allocate
//    heavily (real Smalltalk) while the other workers idle-wait on the pool condvar. The
//    allocating fibers' scavenges + full GCs must stop the world, which requires every
//    idle worker to count as SAFE (heapGcEnterBlocked before the cond_wait). Without it,
//    heapGcBegin waits forever for an idle worker -> the pool hangs -> watchdog FAIL. It
//    also exercises the M4 shared-registry root scan under a real multi-worker pool.
#define SSTW_FIBERS 4
#define SSTW_EXPECTED 200000
static _Atomic int gSstwOk;
static Object *gSstwBlockH;
static void sstwFiber(void *arg)
{
	(void) arg;
	Value r = runBlockOnce(getTaggedPtr(gSstwBlockH)); // allocates 200k Arrays -> scavenges + full GC
	if (!(valueTypeOf(r, VALUE_INT) && (long) asCInt(r) == SSTW_EXPECTED)) {
		__atomic_store_n(&gSstwOk, 0, __ATOMIC_RELAXED);
	}
}

static int schedStwSelfTest(void)
{
	int workers = schedWorkerCount();
	__atomic_store_n(&gSstwOk, 1, __ATOMIC_RELAXED);
	// Persistent handle (survives schedulerRun swapping handleScopes per fiber). Force
	// promotion + full GC via a low threshold so BOTH collectors run under the pool.
	{
		HandleScope setup;
		openHandleScope(&setup); // evalObject/compile needs an open scope
		gSstwBlockH = handle(asObject(evalObject(
			"[ | acc | acc := OrderedCollection new. 1 to: 200000 do: [:i | acc add: (Array with: i with: i)]. acc size ]")));
		runBlockOnce(getTaggedPtr(gSstwBlockH)); // warm up JIT on main before the pool runs
		closeHandleScope(&setup, NULL);
	}
	CurrentThread.heap->oldGcThreshold = 1 * 1024 * 1024;
	unsigned long fullGcBefore = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED);

	schedulerInit();
	for (long i = 0; i < SSTW_FIBERS; i++) {
		schedulerSpawnC(sstwFiber, (void *) i, 0);
	}
	__atomic_store_n(&gWatchdogDone, 0, __ATOMIC_RELEASE);
	pthread_t wd;
	pthread_create(&wd, NULL, schedWatchdog, (void *) 180000L);
	schedulerRun();
	__atomic_store_n(&gWatchdogDone, 1, __ATOMIC_RELEASE);
	pthread_join(wd, NULL);

	unsigned long fullGcs = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED) - fullGcBefore;
	// The point of this test is idle-worker GC-safety, which only exists at workers>1;
	// there the concurrent allocation pressure also reliably trips the full GC. At 1
	// worker (no idle workers, sequential fibers) just require correctness.
	int correct = __atomic_load_n(&gSstwOk, __ATOMIC_RELAXED);
	int ok = correct && (workers > 1 ? fullGcs > 0 : 1);
	fprintf(stderr, "sched-STW self-test: %d allocating fibers on %d workers (rest idle-parked) | full GCs=%lu | all-correct=%d -> %s\n",
		SSTW_FIBERS, workers, fullGcs, correct, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

// 5. ST_SCHED_MIGRATE_TEST — a fiber that PARKS mid-Smalltalk (holding a live reified block
//    context) and RESUMES on ANOTHER worker must allocate into the NEW worker's TLAB (B2.5).
//    Each fiber runs a block that allocates, yields (a real send -> parks with its context
//    live), allocates more, then checksums. With many fibers on many workers they migrate on
//    every yield. WITHOUT the context rebind, the migrated fiber allocates via the parking
//    worker's TLAB while that worker allocates too -> overlapping objects -> wrong checksum /
//    crash. WITH it -> every checksum is exact.
#define SMIG_FIBERS 8
#define SMIG_EXPECTED 320800   // sum i=1..400 of (i + 3i) = 4 * 400*401/2
static _Atomic int gSmigOk;
static Object *gSmigBlockH;
static void smigFiber(void *arg)
{
	(void) arg;
	// Use a SCOPE handle (lives in the fiber's handleScopes, which migrate with it), NOT a
	// persistent handle() — the persistent list is per-worker TLS and would be left on the
	// parking worker when this fiber yields and resumes elsewhere. (runBlockOnce uses a
	// persistent handle, which is fine for non-migrating tests but not here.)
	HandleScope scope;
	openHandleScope(&scope);
	Object *h = scopeHandle(gSmigBlockH->raw);
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, h);
	Value r = sendMessage(getSymbol("value"), &args);
	if (!(valueTypeOf(r, VALUE_INT) && (long) asCInt(r) == SMIG_EXPECTED)) {
		__atomic_store_n(&gSmigOk, 0, __ATOMIC_RELAXED);
	}
	closeHandleScope(&scope, NULL);
}

static int schedMigrateSelfTest(void)
{
	int workers = schedWorkerCount();
	__atomic_store_n(&gSmigOk, 1, __ATOMIC_RELAXED);
	{
		HandleScope setup;
		openHandleScope(&setup);
		// hasContext block (contains inner blocks) that yields mid-iteration.
		gSmigBlockH = handle(asObject(evalObject(
			"[ | acc sum | acc := OrderedCollection new. "
			"  1 to: 400 do: [:i | acc add: (Array with: i with: i * 3). "
			"    (i \\\\ 40) = 0 ifTrue: [ Processor yield ] ]. "
			"  sum := 0. acc do: [:e | sum := sum + (e at: 1) + (e at: 2) ]. sum ]")));
		runBlockOnce(getTaggedPtr(gSmigBlockH)); // warm up JIT on main before the pool runs
		closeHandleScope(&setup, NULL);
	}

	schedulerInit();
	for (long i = 0; i < SMIG_FIBERS; i++) {
		schedulerSpawnC(smigFiber, (void *) i, 0);
	}
	__atomic_store_n(&gWatchdogDone, 0, __ATOMIC_RELEASE);
	pthread_t wd;
	pthread_create(&wd, NULL, schedWatchdog, (void *) 120000L);
	schedulerRun();
	__atomic_store_n(&gWatchdogDone, 1, __ATOMIC_RELEASE);
	pthread_join(wd, NULL);

	int ok = __atomic_load_n(&gSmigOk, __ATOMIC_RELAXED);
	fprintf(stderr, "sched-migrate self-test: %d fibers yielded mid-Smalltalk across %d workers (context rebind) | all-correct=%d -> %s\n",
		SMIG_FIBERS, workers, ok, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

// 6. ST_SCHED_EXC_TEST — on:do: works PER WORKER (the exception-handler chain must be the
//    running worker's, not the codegen thread's). Each fiber loops installing an on:do:,
//    signalling inside it, catching, and yielding (so it migrates). With the handler chain
//    baked to one thread's TLS slot, workers clobber each other's chains -> exceptions go
//    uncaught / find the wrong handler -> wrong count or crash. Per-worker chain -> exact.
#define SEXC_FIBERS 24
#define SEXC_EXPECTED 1000  // one caught per iteration, i in 1..1000
static _Atomic int gSexcOk;
static _Atomic int gSexcDone;   // fibers that RAN TO COMPLETION (an uncaught exception kills a fiber
                                // mid-run, so it never gets here — that is how a broken handler chain shows)
static Object *gSexcBlockH;
static void sexcFiber(void *arg)
{
	(void) arg;
	HandleScope scope;
	openHandleScope(&scope);
	Object *h = scopeHandle(gSexcBlockH->raw);
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, h);
	Value r = sendMessage(getSymbol("value"), &args);
	if (!(valueTypeOf(r, VALUE_INT) && (long) asCInt(r) == SEXC_EXPECTED)) {
		__atomic_store_n(&gSexcOk, 0, __ATOMIC_RELAXED);
	}
	__atomic_add_fetch(&gSexcDone, 1, __ATOMIC_RELAXED);
	closeHandleScope(&scope, NULL);
}

static int schedExcSelfTest(void)
{
	int workers = schedWorkerCount();
	__atomic_store_n(&gSexcOk, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&gSexcDone, 0, __ATOMIC_RELAXED);
	{
		HandleScope setup;
		openHandleScope(&setup);
		// The yield is INSIDE the protected block, BEFORE the signal: the handler is
		// installed on one worker and the exception is raised after a possible migration
		// to another — so the on:do: chain must travel with the fiber and be the RUNNING
		// worker's, not the codegen thread's.
		gSexcBlockH = handle(asObject(evalObject(
			"[ | caught | caught := 0. "
			"  1 to: 1000 do: [:i | "
			"    [ Processor yield. Array new: 8. Error signal ] "
			"      on: Error do: [:e | caught := caught + 1 ] ]. "
			"  caught ]")));
		runBlockOnce(getTaggedPtr(gSexcBlockH)); // warm up JIT on main before the pool runs
		closeHandleScope(&setup, NULL);
	}

	schedulerInit();
	for (long i = 0; i < SEXC_FIBERS; i++) {
		schedulerSpawnC(sexcFiber, (void *) i, 0);
	}
	__atomic_store_n(&gWatchdogDone, 0, __ATOMIC_RELEASE);
	pthread_t wd;
	pthread_create(&wd, NULL, schedWatchdog, (void *) 120000L);
	schedulerRun();
	__atomic_store_n(&gWatchdogDone, 1, __ATOMIC_RELEASE);
	pthread_join(wd, NULL);

	int done = __atomic_load_n(&gSexcDone, __ATOMIC_RELAXED);
	int ok = __atomic_load_n(&gSexcOk, __ATOMIC_RELAXED) && done == SEXC_FIBERS;
	fprintf(stderr, "sched-exception self-test: %d fibers ran on:do:+signal across %d workers (per-worker handler chain) | completed=%d/%d all-correct=%d -> %s\n",
		SEXC_FIBERS, workers, done, SEXC_FIBERS, __atomic_load_n(&gSexcOk, __ATOMIC_RELAXED), ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}


// Snapshot format primitives: shape/object-header encodings must round-trip
// exactly, and the image header must accept a same-host image and REFUSE
// legacy (no magic) and foreign-endian images with an actionable error. Runs
// without a heap, so it also runs on an emulated foreign arch with a stub JIT
// backend (the whole point: it validates the format code on big-endian hosts).
static int snapshotFormatSelfTest(void)
{
	int failures = 0;

	// shape round-trip over a spread of field values (incl. the 16-bit size)
	InstanceShape shapes[] = {
		{ 0, 0, 0, 0, 0, 0, 0 },
		{ 3, 1, 0, 0, 0, 2, 0 },
		{ 255, 255, 255, 1, 1, 255, 0xABCD },
		{ 1, 2, 3, 1, 0, 4, 512 },
	};
	for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
		uint64_t bits = snapshotEncodeShape(shapes[i]);
		InstanceShape back = snapshotDecodeShape(bits);
		if (memcmp(&back, &shapes[i], sizeof(InstanceShape)) != 0) {
			printf("snapshot-format: shape %zu did not round-trip (bits 0x%llx)\n",
				i, (unsigned long long) bits);
			failures++;
		}
	}

	// object-header round-trip
	uint32_t hash;
	uint8_t payloadSize, varsSize;
	snapshotDecodeObjectHeader(snapshotEncodeObjectHeader(0xDEADBEEF, 7, 200),
		&hash, &payloadSize, &varsSize);
	if (hash != 0xDEADBEEF || payloadSize != 7 || varsSize != 200) {
		printf("snapshot-format: object header did not round-trip\n");
		failures++;
	}

	// image header: accept own, refuse legacy and foreign-endian
	char err[256];
	FILE *f = tmpfile();
	snapshotWriteHeader(f);
	rewind(f);
	if (snapshotCheckHeader(f, err, sizeof(err)) != 0) {
		printf("snapshot-format: own header rejected: %s\n", err);
		failures++;
	}
	rewind(f);
	fwrite("JUNK", 1, 4, f); // clobber the magic -> legacy/corrupt
	rewind(f);
	if (snapshotCheckHeader(f, err, sizeof(err)) == 0) {
		printf("snapshot-format: junk magic ACCEPTED?!\n");
		failures++;
	}
	fclose(f);

	f = tmpfile();
	snapshotWriteHeader(f);
	rewind(f);
	uint8_t header[8];
	size_t r = fread(header, 1, 8, f);
	(void) r;
	header[5] = header[5] == 1 ? 2 : 1; // flip declared endianness
	rewind(f);
	fwrite(header, 1, 8, f);
	rewind(f);
	if (snapshotCheckHeader(f, err, sizeof(err)) == 0) {
		printf("snapshot-format: foreign-endian image ACCEPTED?!\n");
		failures++;
	}
	fclose(f);

	printf(failures == 0 ? "snapshot format self-test PASSED\n"
	                     : "snapshot format self-test FAILED (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}


int selfTestFromEnv(char *snapshotFileName, char *bootstrapDir,
	void (*bootstrap)(char *snapshotFileName, char *bootstrapDir))
{
	// Snapshot format primitives (C-level, no image/heap): ST_SNAPSHOT_FORMAT_TEST=1 ./st
	if (getenv("ST_SNAPSHOT_FORMAT_TEST") != NULL) {
		return snapshotFormatSelfTest();
	}

	// ABI emission golden test (C-level, no image): ST_ABI_EMIT_TEST=1 ./st
	// (=print regenerates the expected vectors)
	if (getenv("ST_ABI_EMIT_TEST") != NULL) {
		return abiEmitGoldenSelfTest(getenv("ST_ABI_EMIT_TEST"));
	}

	// Multicore safepoint handshake self-test (C-level): ST_SAFEPOINT_TEST=1 ./st
	if (getenv("ST_SAFEPOINT_TEST") != NULL) {
		return safepointSelfTest();
	}

	// Concurrent shared-heap allocation self-test (C-level): ST_TLAB_TEST=1 ./st
	if (getenv("ST_TLAB_TEST") != NULL) {
		return tlabConcurrencySelfTest();
	}

	// Phase 2 message-serializer self-test (needs the image): ST_MESSAGE_TEST=1 -s snap
	if (getenv("ST_MESSAGE_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		schedulerInit();
		int msgResult = 0;
		schedulerSpawnC(runMessageTest, &msgResult, 0);
		schedulerRun();
		freeHandles();
		freeThread(&CurrentThread);
		return msgResult;
	}

	// Worker-thread Smalltalk execution self-test (needs the image): ST_WORKER_TEST=1 -s snap
	if (getenv("ST_WORKER_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return workerSelfTest();
	}

	// Parallel Smalltalk execution across cores: ST_PARALLEL_TEST=1 -s snap
	if (getenv("ST_PARALLEL_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return parallelSelfTest();
	}

	// Parallel Smalltalk WITH allocation + safepoint-coordinated GC: ST_PARGC_TEST=1 -s snap
	if (getenv("ST_PARGC_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return parallelGcSelfTest();
	}

	// Parallel Smalltalk WITH promotion + multi-thread FULL GC: ST_PARFULLGC_TEST=1 -s snap
	if (getenv("ST_PARFULLGC_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return parallelFullGcSelfTest();
	}

	// Scheduler-active collector must scan every mutator (B0): ST_SCHEDGC_TEST=1 -s snap
	if (getenv("ST_SCHEDGC_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedGcSelfTest();
	}

	// JIT back-edge safepoint poll: CPU-bound loops must yield (B1): ST_SAFEPOINT_JIT_TEST=1 -s snap
	if (getenv("ST_SAFEPOINT_JIT_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return safepointJitSelfTest();
	}

	// JIT back-edge stackmap correctness / loop-carried root (B1 Q4): ST_SAFEPOINT_ROOTS_TEST=1 -s snap
	if (getenv("ST_SAFEPOINT_ROOTS_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return safepointRootsSelfTest();
	}

	// B2.3 multi-worker scheduler pool (set ST_SCHED_WORKERS>1): CPU-bound fibers run
	// across the pool; no fiber double-runs; no wakeup is lost; idle workers are GC-safe.
	if (getenv("ST_SCHED_PARALLEL_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedParallelSelfTest();
	}
	if (getenv("ST_SCHED_DOUBLERUN_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedDoubleRunSelfTest();
	}
	if (getenv("ST_SCHED_LOSTWAKE_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedLostWakeSelfTest();
	}
	if (getenv("ST_SCHED_STW_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedStwSelfTest();
	}
	if (getenv("ST_SCHED_MIGRATE_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedMigrateSelfTest();
	}
	if (getenv("ST_SCHED_EXC_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrap(snapshotFileName, bootstrapDir);
		return schedExcSelfTest();
	}

	return -1; // no ST_*_TEST env var set: run the normal program
}
