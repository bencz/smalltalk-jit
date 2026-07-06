#include "vm/Bootstrap.h"
#include "vm/Snapshot.h"
#include "vm/Entry.h"
#include "vm/Repl.h"
#include "vm/Thread.h"
#include "vm/Scheduler.h"
#include "vm/Isolate.h"
#include "vm/Cli.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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


int main(int argc, char **args)
{
	CliArgs cliArgs;
	ProgramContext ctx = { .cliArgs = &cliArgs, .result = EXIT_SUCCESS };

	parseCliArgs(&cliArgs, argc, args);

	// Phase 2 transport self-test (C-level): ST_TRANSPORT_TEST=1 ./st
	if (getenv("ST_TRANSPORT_TEST") != NULL) {
		return isolateTransportSelfTest();
	}

	// Multi-isolate mode (-i N): each isolate boots its own independent VM on its
	// own OS thread (own heap/GC/scheduler/kernel), loading -s <snapshot> and
	// evaluating -e <program>. The main thread just spawns and joins them.
	if (cliArgs.isolates > 1) {
		return isolatesRun(cliArgs.isolates, cliArgs.snapshotFileName, cliArgs.eval);
	}

	initThread(&CurrentThread);
	bootstrapSmalltalk(cliArgs.snapshotFileName, cliArgs.bootstrapDir);

	// Hand execution over to the cooperative fiber scheduler: the program runs
	// as the first fiber and the loop keeps running until it (and any processes
	// it forked) are done.
	schedulerInit();
	schedulerSpawnC(runProgram, &ctx, 0);
	schedulerRun();

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
