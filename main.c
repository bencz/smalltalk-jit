#include "vm/Bootstrap.h"
#include "vm/Snapshot.h"
#include "vm/Entry.h"
#include "vm/Repl.h"
#include "vm/Thread.h"
#include "vm/Scheduler.h"
#include "vm/Isolate.h"
#include "vm/Message.h"
#include "vm/Safepoint.h"
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


static void runMessageTest(void *arg)
{
	*(int *) arg = messageSelfTest();
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
