#ifndef CLI_H
#define CLI_H

#include "vm/os/Os.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
	char *error;
	char operand;
	char *bootstrapDir;
	char *snapshotFileName;
	_Bool snapshotExplicit;
	char *fileName;
	char *eval;
	_Bool printHelp;
	// Project subcommand (st new/build/run/test/repl/help); NULL = legacy mode.
	char *subcommand;
	// Leading argv words the subcommand consumed. main.c locates getopt's
	// leftover (script-visible) args at argv + argShift + optind.
	int argShift;
	// argc after subcommand-level flags (--force) were filtered out of argv.
	int argcAdjusted;
	// st build/run/test --force: rebuild regardless of staleness.
	_Bool force;
	// Backing storage when the snapshot search resolves to the executable-
	// adjacent image (resolveSnapshotPath below).
	char snapshotPathBuffer[4096];
} CliArgs;


static _Bool cliIsSubcommand(char *word)
{
	static const char *subcommands[] = {"new", "build", "run", "test", "repl", "help", NULL};
	for (int i = 0; subcommands[i] != NULL; i++) {
		if (strcmp(word, subcommands[i]) == 0) {
			return 1;
		}
	}
	return 0;
}


static void parseCliArgs(CliArgs *cliArgs, int argc, char **args)
{
	cliArgs->error = NULL;
	cliArgs->operand = '0';
	cliArgs->bootstrapDir = NULL;
	cliArgs->snapshotFileName = "snapshot";
	cliArgs->snapshotExplicit = 0;
	cliArgs->fileName = NULL;
	cliArgs->eval = NULL;
	cliArgs->printHelp = 0;
	cliArgs->subcommand = NULL;
	cliArgs->argShift = 0;
	cliArgs->argcAdjusted = argc;
	cliArgs->force = 0;

	// Project subcommands sit in argv[1], before any option flags. Anything
	// else in argv[1] falls through to the legacy getopt surface untouched:
	// a stray first word was never an error and stays that way.
	if (argc > 1 && args[1][0] != '-' && cliIsSubcommand(args[1])) {
		cliArgs->subcommand = args[1];
		cliArgs->argShift = 1;
		// Subcommand-level flags are filtered out of argv before getopt (which
		// only understands single-letter options) and before the script-visible
		// command line is captured.
		int write = 2;
		for (int read = 2; read < argc; read++) {
			if (strcmp(args[read], "--force") == 0) {
				cliArgs->force = 1;
				continue;
			}
			args[write++] = args[read];
		}
		cliArgs->argcAdjusted = write;
	}

	int arg;
	opterr = 0;
	while ((arg = getopt(cliArgs->argcAdjusted - cliArgs->argShift, args + cliArgs->argShift, "hb:s:f:e:")) != -1) {
		switch (arg) {
		case 'e':
			cliArgs->eval = optarg;
			break;
		case 'f':
			cliArgs->fileName = optarg;
			break;
		case 's':
			cliArgs->snapshotFileName = optarg;
			cliArgs->snapshotExplicit = 1;
			break;
		case 'b':
			cliArgs->bootstrapDir = optarg;
			break;
		case 'h':
			cliArgs->printHelp = 1;
			break;
		case '?':
			cliArgs->operand = optopt;
			switch (optopt) {
			case 'e':
			case 'f':
			case 's':
			case 'b':
				cliArgs->error = "Option -%c requires an operand";
				break;
			default:
				cliArgs->error = "Unrecognized option: '-%c'";
				cliArgs->operand = optopt;
				break;
			}
			break;
		}
	}
	opterr = 1;

	if (cliArgs->eval != NULL && cliArgs->fileName != NULL) {
		cliArgs->error = "Cannot use -e and -f together";
	}
	if (cliArgs->subcommand != NULL
			&& (cliArgs->eval != NULL || cliArgs->fileName != NULL || cliArgs->bootstrapDir != NULL)) {
		cliArgs->error = "Cannot combine a subcommand with -e, -f or -b";
	}
}


// Resolve which snapshot to load when -s was not given. Search order:
//   1. the ST_IMAGE environment variable
//   2. subcommand mode only: "snapshot" next to the st executable, then in
//      its parent directory (installed layout, then the dev tree where the
//      executable is build/st and the base image sits at the repo root)
//   3. "snapshot" in the current directory (the historical default)
// Legacy mode skips step 2 so existing flag-based workflows keep their exact
// behavior. Bootstrap runs (-b) always write to the explicit or historical
// path: an environment variable must never redirect a WRITE target.
static void resolveSnapshotPath(CliArgs *cliArgs)
{
	if (cliArgs->snapshotExplicit || cliArgs->bootstrapDir != NULL) {
		return;
	}
	char *envImage = getenv("ST_IMAGE");
	if (envImage != NULL && envImage[0] != '\0') {
		cliArgs->snapshotFileName = envImage;
		return;
	}
	if (cliArgs->subcommand != NULL
			&& osExecutablePath(cliArgs->snapshotPathBuffer, sizeof(cliArgs->snapshotPathBuffer))) {
		for (int hop = 0; hop < 2; hop++) {
			char *slash = strrchr(cliArgs->snapshotPathBuffer, '/');
			if (slash == NULL || slash == cliArgs->snapshotPathBuffer) {
				break;
			}
			*slash = '\0'; // drop the last component (st, then its directory)
			size_t dirLength = strlen(cliArgs->snapshotPathBuffer);
			if (dirLength + sizeof("/snapshot") > sizeof(cliArgs->snapshotPathBuffer)) {
				break;
			}
			strcpy(cliArgs->snapshotPathBuffer + dirLength, "/snapshot");
			if (access(cliArgs->snapshotPathBuffer, R_OK) == 0) {
				cliArgs->snapshotFileName = cliArgs->snapshotPathBuffer;
				return;
			}
			cliArgs->snapshotPathBuffer[dirLength] = '\0'; // undo for the next hop
		}
	}
}


static void printCliHelp(void)
{
	printf(
		"Usage:\t<executable> [<command>] [-e <code>] [-f <file>] [-s <snapshot file>] [-b <core package dir>]\n"
		"Commands:\n"
		"\tnew <name>  scaffold a new project\n"
		"\tbuild       compile the project into .stbuild/program.img (--force rebuilds)\n"
		"\trun [<f>.st] build if stale, then run the project entry point; with a\n"
		"\t            script operand, run the script in ITS project's image\n"
		"\t            (found from the script's directory; base image outside one)\n"
		"\ttest        build if stale, then run the project tests\n"
		"\trepl        REPL; inside a project, in the project image and namespace\n"
		"\thelp        print this help\n"
		"Options:\n"
		"\t-e evaluate code\n"
		"\t-f compile classes and evaluate code within specified file\n"
		"\t-s path to snapshot file\n"
		"\t-b bootstrap from the core package directory (normally packages/Core)\n"
		"\t-h prints this help\n"
		"Snapshot search when -s is absent: ST_IMAGE env var, then (commands\n"
		"only) the snapshot next to the executable or in its parent dir, then\n"
		"./snapshot\n"
	);
}

#endif
