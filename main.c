#include "vm/Bootstrap.h"
#include "vm/Snapshot.h"
#include "vm/Entry.h"
#include "vm/Repl.h"
#include "vm/Thread.h"
#include "vm/Scheduler.h"
#include "vm/Isolate.h"
#include "vm/Message.h"
#include "vm/Safepoint.h"
#include "vm/Handle.h"
#include "vm/Smalltalk.h"
#include "vm/Heap.h"
#include "vm/Os.h"
#include "vm/Cli.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

static void bootstrapSmalltalk(char *snapshotFileName, char *bootstrapDir);

typedef struct {
	CliArgs *cliArgs;
	int result;
} ProgramContext;


// Runs the user program on the main fiber. Everything the program does
// (compiling, evaluating, the REPL) executes as fiber #0 so that forked
// processes can be scheduled cooperatively alongside it.
static void runProgram(void *arg)
{
	ProgramContext *ctx = arg;
	CliArgs *cliArgs = ctx->cliArgs;

	if (cliArgs->error != NULL) {
		printf(cliArgs->error, cliArgs->operand);
		printf("\n");
		ctx->result = EXIT_FAILURE;
	} else if (cliArgs->printHelp) {
		printCliHelp();
	} else if (cliArgs->fileName != NULL) {
		Value blockResult;
		if (parseFileAndInitialize(cliArgs->fileName, &blockResult)) {
			ctx->result = valueTypeOf(blockResult, VALUE_INT) ? asCInt(blockResult) : ctx->result;
		} else {
			ctx->result = EXIT_FAILURE;
		}
	} else if (cliArgs->eval != NULL) {
		ctx->result = asCInt(evalCode(cliArgs->eval));
	} else {
		runRepl();
	}
}


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
static Value gWorkerBlock;
static Value gWorkerResult;

static void *workerRunBlock(void *arg)
{
	(void) arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = gWorkerHeap;
	initRememberedSet(&CurrentThread.rememberedSet);
	CurrentThread.nextMutator = NULL;
	heapAddMutator(gWorkerHeap, &CurrentThread);
	Handles = gMainHandles; // well-known handles point at shared (old-space) objects
	initThreadContext(&CurrentThread);               // root context on the worker's TLAB
	HandleScope scope;
	openHandleScope(&scope);                         // entry points below re-handle into a parent
	Object *blockH = handle(asObject(gWorkerBlock)); // root the block before we allocate
	EntryArgs args = { .size = 0 };
	entryArgsAdd(&args, getTaggedPtr(blockH));        // re-read (a GC may have moved it)
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
	gWorkerBlock = evalObject("[ | n | n := 0. 1 to: 200000 do: [:i | n := n + (Array new: 2) size]. n ]");
	handle(asObject(gWorkerBlock)); // keep it alive on the main mutator across the spawn

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
	Handles = gMainHandles;
	initThreadContext(&CurrentThread);
	HandleScope scope;
	openHandleScope(&scope);
	Object *blockH = handle(asObject(gWorkerBlock));
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
	entryArgsAdd(&args, getTaggedPtr(h));
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
	gWorkerBlock = evalObject("[ | s | s := 0. 1 to: 30000000 do: [:i | s := s + i]. s ]");
	handle(asObject(gWorkerBlock));
	long expected = 450000015000000L; // 30000000 * 30000001 / 2

	runBlockOnce(gWorkerBlock); // warm up JIT compilation so workers only READ cached code
	int64_t s0 = osCurrentMicroTime();
	runBlockOnce(gWorkerBlock);  // serial baseline: one run
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
	gWorkerBlock = evalObject("[ | n | n := 0. 1 to: 400000 do: [:i | n := n + (Array new: 2) size]. n ]");
	handle(asObject(gWorkerBlock));
	long expected = 800000; // 2 * 400000
	runBlockOnce(gWorkerBlock); // warm up JIT compilation

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
	gWorkerBlock = evalObject(
		"[ | acc sum | "
		"  acc := OrderedCollection new. "
		"  1 to: 100000 do: [:i | acc add: (Array with: i with: i * 2) ]. "
		"  sum := 0. "
		"  acc do: [:a | sum := sum + (a at: 1) + (a at: 2) ]. "
		"  sum ]");
	handle(asObject(gWorkerBlock));
	long expected = 15000150000L; // sum over i=1..100000 of (i + 2i) = 3 * 100000*100001/2
	unsigned long fullGcBefore = __atomic_load_n(&gFullGcRuns, __ATOMIC_RELAXED);
	runBlockOnce(gWorkerBlock); // warm up JIT compilation so workers only READ cached code

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


int main(int argc, char **args)
{
	CliArgs cliArgs;
	ProgramContext ctx = { .cliArgs = &cliArgs, .result = EXIT_SUCCESS };

	parseCliArgs(&cliArgs, argc, args);

	// Phase 2 transport self-test (C-level): ST_TRANSPORT_TEST=1 ./st
	if (getenv("ST_TRANSPORT_TEST") != NULL) {
		return isolateTransportSelfTest();
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
		bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);
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
		bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);
		return workerSelfTest();
	}

	// Parallel Smalltalk execution across cores: ST_PARALLEL_TEST=1 -s snap
	if (getenv("ST_PARALLEL_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);
		return parallelSelfTest();
	}

	// Parallel Smalltalk WITH allocation + safepoint-coordinated GC: ST_PARGC_TEST=1 -s snap
	if (getenv("ST_PARGC_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);
		return parallelGcSelfTest();
	}

	// Parallel Smalltalk WITH promotion + multi-thread FULL GC: ST_PARFULLGC_TEST=1 -s snap
	if (getenv("ST_PARFULLGC_TEST") != NULL) {
		initThread(&CurrentThread);
		bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);
		return parallelFullGcSelfTest();
	}

	initThread(&CurrentThread);
	bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);

	// The program runs on the MAIN isolate (id 0). It can spawn worker isolates
	// at runtime (`Isolate spawn:`), each a fresh VM on another core that reloads
	// this same image; the main isolate keeps running. Give it an inbox so
	// workers can message it, and remember the image path so workers can reload.
	isolateSetSnapshotPath(cliArgs.snapshotFileName);
	schedulerInit();
	isolateInboxInit(0);

	// Hand execution over to the cooperative fiber scheduler: the program runs
	// as the first fiber and the loop keeps running until it (and any processes
	// it forked) are done.
	schedulerSpawnC(runProgram, &ctx, 0);
	schedulerRun();

	isolateJoinWorkers(); // wait for any worker isolates the program spawned

	freeHandles();
	freeThread(&CurrentThread);
	return ctx.result;
}



static void bootstrapSmalltalk(char *snapshotFileName, char *bootstrapDir)
{
	FILE *snapshot;
	if (bootstrapDir) {
		snapshot = fopen(snapshotFileName, "w+");
		if (snapshot == NULL) {
			printf("Cannot write to snapshot file: '%s'\n", snapshotFileName);
			exit(EXIT_FAILURE);
		}
		if (!bootstrap(bootstrapDir)) {
			printf("Bootstrap failed\n");
			exit(EXIT_FAILURE);
		}
		snapshotWrite(snapshot);
	} else {
		snapshot = fopen(snapshotFileName, "r");
		if (snapshot == NULL) {
			printf("Cannot read snapshot file: '%s'\n", snapshotFileName);
			exit(EXIT_FAILURE);
		}
		snapshotRead(snapshot);
	}
	fclose(snapshot);
}
