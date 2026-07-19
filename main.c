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
#include "vm/tools/Project.h"
#include "vm/memory/GarbageCollector.h"
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


// Pre-flight for the project subcommands, BEFORE any image I/O: build/run/
// test require a project root (package.st found walking upward); repl runs
// in the project when there is one and as the plain base REPL otherwise.
static _Bool planProject(CliArgs *cliArgs, ProjectPlan *plan)
{
	char *subcommand = cliArgs->subcommand;
	_Bool wantsProject = strcmp(subcommand, "build") == 0 || strcmp(subcommand, "run") == 0
		|| strcmp(subcommand, "test") == 0 || strcmp(subcommand, "repl") == 0;
	if (!wantsProject) {
		return 1;
	}
	if (!projectFindRoot(plan->root, sizeof(plan->root))) {
		if (strcmp(subcommand, "repl") == 0) {
			return 1;
		}
		printf("st %s: no %s found from the current directory upward\n",
			subcommand, PROJECT_MANIFEST);
		return 0;
	}
	plan->hasProject = 1;
	projectBuildPath(plan->image, sizeof(plan->image), plan->root, PROJECT_IMAGE);
	plan->force = cliArgs->force;
	plan->stale = plan->force || projectIsStale(plan->root, cliArgs->snapshotFileName);
	return 1;
}


// st test: the image decides WHICH files run (ProjectTool prepareTests
// answers newline-joined paths and points the session default namespace at
// <Root>Tests); each file then goes through the exact -f path, so TestRun
// test files work unchanged inside projects. Exit code = summed fail counts.
static int runProjectTests(void)
{
	static char paths[65536];
	if (!projectEvalToCString("ProjectTool prepareTests", paths, sizeof(paths))) {
		return EXIT_FAILURE;
	}
	int total = 0;
	char *cursor = paths;
	while (*cursor != '\0') {
		char *newline = strchr(cursor, '\n');
		if (newline != NULL) {
			*newline = '\0';
		}
		if (*cursor != '\0') {
			printf("test %s\n", cursor);
			fflush(stdout); // Smalltalk output below writes unbuffered
			Value blockResult = tagInt(0);
			if (!parseFileAndInitialize(cursor, &blockResult)) {
				total += 1;
			} else if (valueTypeOf(blockResult, VALUE_INT)) {
				total += (int) asCInt(blockResult);
			}
		}
		if (newline == NULL) {
			break;
		}
		cursor = newline + 1;
	}
	return total;
}


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
	} else if (cliArgs->subcommand != NULL && strcmp(cliArgs->subcommand, "repl") != 0) {
		// Project subcommands: thin eval bridges into ProjectTool
		// (smalltalk/Packages/ProjectTool.st); `st repl` falls through to the
		// ordinary REPL below, already inside the project image when one is
		// loaded. Uncaught errors still poison the exit code through the
		// scheduler's unhandled-error accounting.
		if (strcmp(cliArgs->subcommand, "new") == 0) {
			ctx->result = projectEvalToInt("ProjectTool scaffold");
		} else if (strcmp(cliArgs->subcommand, "run") == 0) {
			ctx->result = projectEvalToInt("ProjectTool run");
		} else if (strcmp(cliArgs->subcommand, "test") == 0) {
			ctx->result = runProjectTests();
		} else {
			ctx->result = EXIT_FAILURE;
		}
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
	resolveSnapshotPath(&cliArgs);

	// Subcommand early dispatch (vm/tools/Cli.h). `st help`, argument errors
	// and the project pre-flight (root discovery + staleness) all answer
	// before any image is loaded; `st build` on a fresh project never boots a
	// heap at all.
	ProjectPlan plan = { 0 };
	if (cliArgs.subcommand != NULL) {
		if (cliArgs.error != NULL) {
			printf(cliArgs.error, cliArgs.operand);
			printf("\n");
			return EXIT_FAILURE;
		}
		if (strcmp(cliArgs.subcommand, "help") == 0) {
			printCliHelp();
			return EXIT_SUCCESS;
		}
		if (!planProject(&cliArgs, &plan)) {
			return EXIT_FAILURE;
		}
		if (strcmp(cliArgs.subcommand, "build") == 0 && !plan.stale) {
			printf("up to date\n");
			return EXIT_SUCCESS;
		}
	}

	// Script-visible command line (CommandLinePrimitive): everything left after
	// the subcommand word (if any) and the options, e.g. `st -f prog.st alpha
	// beta` -> #('alpha' 'beta').
	primitivesSetCommandLine(cliArgs.argcAdjusted - cliArgs.argShift - optind,
		args + cliArgs.argShift + optind);

	// The C-level self-test battery (ST_*_TEST env vars) lives in
	// vm/tests/SelfTests.c; -1 means "no self-test requested".
	{
		int selfTest = selfTestFromEnv(cliArgs.snapshotFileName, cliArgs.bootstrapDir, bootstrapSmalltalk);
		if (selfTest >= 0) {
			return selfTest;
		}
	}

	initThread(&CurrentThread);
	// Project runs against a FRESH cache load the built project image; a stale
	// (or first) build loads the base image and rebuilds below. Everything
	// else loads the resolved base image.
	{
		char *image = cliArgs.snapshotFileName;
		if (plan.hasProject && !plan.stale) {
			image = plan.image;
		}
		bootstrapSmalltalk(image, cliArgs.bootstrapDir);
	}

	// Stale project: build PRE-scheduler, mirroring the -b bootstrap path, so
	// the image is snapshotted with zero fibers ever started. ProjectTool
	// build loads the package graph, records the entry point and the default
	// namespace IN the image, and answers the output path; C then collects
	// the builder transients and writes the snapshot. st run/test/repl
	// continue in the same process on the freshly built in-memory image.
	if (plan.hasProject && plan.stale) {
		char out[PROJECT_PATH_MAX];
		if (!projectEvalToCString("ProjectTool build", out, sizeof(out))) {
			return EXIT_FAILURE; // the image already printed the build error
		}
		collectGarbage(&CurrentThread);
		FILE *image = fopen(out, "w+");
		if (image == NULL) {
			printf("Cannot write project image: '%s'\n", out);
			return EXIT_FAILURE;
		}
		snapshotWrite(image);
		fclose(image);
		printf("built %s\n", out);
		fflush(stdout); // program output below writes unbuffered
		if (strcmp(cliArgs.subcommand, "build") == 0) {
			return EXIT_SUCCESS;
		}
	}

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
