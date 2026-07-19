#include "vm/tools/Bootstrap.h"
#include "vm/tools/Snapshot.h"
#include "vm/core/Entry.h"
#include "vm/tools/Repl.h"
#include "vm/core/Thread.h"
#include "vm/concurrency/Scheduler.h"
#include "vm/runtime/Message.h"
#include "vm/memory/Safepoint.h"
#include "vm/core/Handle.h"
#include "vm/core/Smalltalk.h"
#include "vm/memory/Heap.h"
#include "vm/core/Exception.h"
#include "vm/os/Os.h"
#include "vm/tools/Cli.h"
#include "vm/runtime/Primitives.h"
#include "vm/jit/TargetCpu.h"
#include "vm/jit/InlineCache.h"
#include "vm/jit/Tier.h"
#include "vm/tests/SelfTests.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

static void bootstrapSmalltalk(char *snapshotFileName, char *bootstrapDir);

typedef struct {
	CliArgs *cliArgs;
	int result;
	_Bool interactive; // REPL session: unhandled errors do not poison the exit code
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
		ctx->interactive = 1;
		runRepl();
	}
}


int main(int argc, char **args)
{
	CliArgs cliArgs;
	ProgramContext ctx = { .cliArgs = &cliArgs, .result = EXIT_SUCCESS };

	// Which CPU MODEL of this architecture are we on (jit/TargetCpu.h). FIRST,
	// before anything else: the JIT reads the answer while emitting, and the
	// self-test dispatch below returns without ever reaching initThread, so
	// anything later would leave the goldens and the JIT self-tests emitting
	// against an undetected CPU. Read-only from here on, hence lock-free and
	// TLS-free for every worker.
	targetCpuDetect();

	parseCliArgs(&cliArgs, argc, args);

	// Script-visible command line (CommandLinePrimitive): everything left after
	// the options, e.g. `st -f prog.st alpha beta` -> #('alpha' 'beta').
	primitivesSetCommandLine(argc - optind, args + optind);

	// The C-level self-test battery (ST_*_TEST env vars) lives in
	// vm/tests/SelfTests.c; -1 means "no self-test requested".
	{
		int selfTest = selfTestFromEnv(cliArgs.snapshotFileName, cliArgs.bootstrapDir, bootstrapSmalltalk);
		if (selfTest >= 0) {
			return selfTest;
		}
	}

	initThread(&CurrentThread);
	bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);

	// Image idempotence probe (scripts/check-image-idempotence.sh): re-save the
	// just-loaded image and exit — a load->save round trip must be a fixpoint.
	char *resavePath = getenv("ST_RESAVE");
	if (resavePath != NULL) {
		FILE *out = fopen(resavePath, "w+");
		if (out == NULL) {
			printf("Cannot write to snapshot file: '%s'\n", resavePath);
			return EXIT_FAILURE;
		}
		snapshotWrite(out);
		fclose(out);
		return EXIT_SUCCESS;
	}

	schedulerInit();

	// Hand execution over to the cooperative fiber scheduler: the program runs
	// as the first fiber and the loop keeps running until it (and any processes
	// it forked) are done.
	schedulerSpawnC(runProgram, &ctx, 0);
	schedulerRun();

	// Inline-cache observability (jit/InlineCache.h): dump the counters at exit
	// under ST_IC_STATS=1 (the same flag that makes the JIT emit the hit/poly
	// increments; without it those two stay zero and the rest still counts).
	if (icStatsEnabled()) {
		icPrintStats();
	}
	// Tier observability (jit/Tier.h): same contract under ST_TIER_STATS=1.
	if (tierStatsEnabled()) {
		tierPrintStats();
	}

	freeHandles();
	freeThread(&CurrentThread);
	// Any fiber that died in Exception>>defaultAction (unhandled error) makes a
	// non-interactive run FAIL, even though the main block's own return value
	// was fine: an uncaught error must never exit 0 (the historical false-pass
	// that let a printed backtrace count as success). The REPL is exempt.
	if (!ctx.interactive && ctx.result == 0 && schedulerUnhandledErrors() > 0) {
		ctx.result = EXIT_FAILURE;
	}
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
