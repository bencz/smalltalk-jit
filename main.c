// st: the VM's command line.
//
// THE VOCABULARY IS KEPT WHOLE and comes from vm/tools/Cli.h, unchanged: the
// subcommands (new, build, run, test, repl, help) and the flags (-e, -f, -s, -b,
// -h) are what the tooling, the scripts and the muscle memory around this
// project already speak, and a port is not a reason to change any of it.
//
// What IS narrow today is how much of it can be carried out. Every subcommand
// except evaluation ends in an operation the dry cut took out and that has not
// been reimplemented yet: reading or writing an image (Snapshot), turning a
// package directory into classes (the class builder), and the REPL loop. Those
// say exactly what is missing and exit non-zero, because a command that
// silently does nothing is worse than one that refuses.

#include "compiler/Compile.h"
#include "compiler/Parser.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "memory/Heap.h"
#include "runtime/Primitive.h"
#include "tools/Bootstrap.h"
#include "tools/Cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What a command needs that does not exist yet. One message, one place, so the
// list of what is missing cannot drift from what the build actually contains.
static int notPortedYet(const char *what, const char *needs)
{
	fprintf(stderr, "st: %s is not ported to jit-v2 yet (it needs %s)\n", what, needs);
	return 3;
}


// Compile `source` as the body of a method and run it with nil as the receiver.
//
// A method and not something special: the front end compiles methods, the tier
// compiles CodeUnits, and an expression typed at the command line is a body with
// no arguments. Making it anything else would be a second path through the
// compiler that nothing else exercises.
static int evaluate(const char *source)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t length = strlen(source) + 32;
	char *wrapped = malloc(length);
	if (wrapped == NULL) {
		closeHandleScope(&scope, NULL);
		return 1;
	}
	snprintf(wrapped, length, "doIt [ %s ]", source);

	Parser parser;
	initParser(&parser, stringFromC(wrapped));
	MethodNode *node = parseMethod(&parser);
	if (node == NULL) {
		printParseError(&parser, "-e");
		freeParser(&parser);
		free(wrapped);
		closeHandleScope(&scope, NULL);
		return 1;
	}

	// No owner class: an expression has no instance variables to resolve
	// against, and every name in it is either its own temporary or a global.
	CompileContext context = { NULL, smalltalkGlobals() };
	CompileError error;
	CodeUnit *unit = compileMethod(node, &context, &error);
	freeParser(&parser);
	free(wrapped);
	if (unit == NULL) {
		fprintf(stderr, "st: %s", compileStatusName(error.status));
		if (error.what != NULL) {
			fprintf(stderr, ": ");
			printRawString(error.what->raw);
		}
		fprintf(stderr, "\n");
		closeHandleScope(&scope, NULL);
		return 1;
	}

	Opcode unsupported = OP_COUNT;
	NativeCode *code = jitCompile(unit, &unsupported);
	if (code == NULL) {
		fprintf(stderr, "st: the tier does not implement %s yet\n",
			opcodeName(unsupported));
		closeHandleScope(&scope, NULL);
		return 1;
	}
	jitCall0(code, tagPtr(Handles.nil.raw));
	closeHandleScope(&scope, NULL);
	return 0;
}


int main(int argc, char **argv)
{
	CliArgs cliArgs;
	parseCliArgs(&cliArgs, argc, argv);
	if (cliArgs.error != NULL) {
		fprintf(stderr, "st: ");
		fprintf(stderr, cliArgs.error, cliArgs.operand);
		fprintf(stderr, "\n");
		printCliHelp();
		return 2;
	}
	if (cliArgs.printHelp
			|| (cliArgs.subcommand != NULL && strcmp(cliArgs.subcommand, "help") == 0)) {
		printCliHelp();
		return 0;
	}
	resolveSnapshotPath(&cliArgs);

	initThread(&CurrentThread);
	initHandles();
	bootstrapBuiltinKernel();

	// The project subcommands. Each one is a real command with a real meaning;
	// what it needs is named, so the gap is a checklist and not a mystery.
	if (cliArgs.subcommand != NULL) {
		const char *name = cliArgs.subcommand;
		if (strcmp(name, "new") == 0) {
			return notPortedYet("st new", "the project scaffolder");
		}
		if (strcmp(name, "build") == 0) {
			return notPortedYet("st build", "the class builder and the image writer");
		}
		if (strcmp(name, "run") == 0) {
			return notPortedYet("st run", "the class builder and the image reader");
		}
		if (strcmp(name, "test") == 0) {
			return notPortedYet("st test", "the class builder and the image reader");
		}
		if (strcmp(name, "repl") == 0) {
			return notPortedYet("st repl", "the read-eval-print loop");
		}
	}

	// -b: build the classes of a package directory on top of the built-in
	// kernel. What is missing to make it an IMAGE is the writer, so this reports
	// what it managed to build and does not pretend to have saved anything.
	if (cliArgs.bootstrapDir != NULL) {
		BootstrapReport report;
		bootstrapLoadPackage(cliArgs.bootstrapDir, &report);
		printf("%zu files, %zu classes, %zu methods, %zu initializers\n",
			report.filesRead, report.classesBuilt, report.methodsBuilt,
			report.initializersRun);
		if (report.error != NULL) {
			fprintf(stderr, "st: %zu class(es) did not build; the first was\n"
				"    %s%s\n    in %s\n", report.classesFailed, report.error,
				report.errorDetail, report.errorFile);
			return 4;
		}
		// The classes are LIVE now, in this process. Writing them out needs the
		// image writer, but RUNNING against them needs nothing else, and that is
		// what makes `-b DIR -e CODE` the cheapest way to find out what the real
		// kernel does when it actually executes.
		if (cliArgs.eval != NULL) {
			return evaluate(cliArgs.eval);
		}
		fprintf(stderr, "st: writing an image is not ported to jit-v2 yet, so "
			"nothing was saved\n");
		return 3;
	}
	if (cliArgs.fileName != NULL) {
		return notPortedYet("running a source file", "the class builder");
	}

	// An image the caller NAMED with -s and that exists is refused rather than
	// ignored: ignoring it would run the built-in kernel while the caller
	// believed their image was loaded.
	//
	// One the SEARCH merely found is a different matter. Every checkout of this
	// project has a `snapshot` left over from before the cut, nobody asked for
	// it, and refusing would make `st -e` unusable in the source tree. It is
	// passed over, with one line on stderr so that which kernel is running is
	// never a guess. A path that does not exist is not an error at all: that is
	// what every run looks like until there is a writer.
	if (cliArgs.snapshotFileName != NULL) {
		FILE *existing = fopen(cliArgs.snapshotFileName, "rb");
		if (existing != NULL) {
			fclose(existing);
			if (cliArgs.snapshotExplicit) {
				fprintf(stderr, "st: reading an image is not ported to jit-v2 yet "
					"(would have read '%s')\n", cliArgs.snapshotFileName);
				return 3;
			}
			fprintf(stderr, "st: ignoring the image at '%s' and running the "
				"built-in kernel; reading an image is not ported to jit-v2 yet\n",
				cliArgs.snapshotFileName);
		}
	}

	if (cliArgs.eval != NULL) {
		return evaluate(cliArgs.eval);
	}
	printCliHelp();
	return 0;
}
