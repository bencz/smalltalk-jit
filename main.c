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
#include "tools/ClassBuilder.h"
#include "tools/Cli.h"
#include "tools/Project.h"
#include "tools/Snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What a command needs that does not exist yet. One message, one place, so the
// list of what is missing cannot drift from what the build actually contains.
// Two forward declarations, because the project subcommands sit at the top of
// this file (where a reader looks for them) and are built out of the evaluation
// and file-running machinery further down.
//
// runFile is the -f path, and `st test` reuses it verbatim rather than having a
// second one: a test file inside a project is an ordinary file of blocks.
static int runFile(const char *path);
static _Bool evalToCString(const char *code, char *buffer, size_t size);
static int evalToInt(const char *code);


static int notPortedYet(const char *what, const char *needs)
{
	fprintf(stderr, "st: %s is not ported to jit-v2 yet (it needs %s)\n", what, needs);
	return 3;
}


// ---- the project subcommands ------------------------------------------------
//
// EVERYTHING PROJECT-SHAPED IS IN THE IMAGE. The manifest, the dependency
// graph, the entry point and the scaffolding all live in
// packages/Core/src/Packages/ProjectTool.st, and what is here is three things
// Smalltalk cannot do for itself: find the root before a heap exists, decide
// staleness by stat'ing files, and write the snapshot once the image is built.
// That split is not new -- it is the one the old VM had -- and it is why `st`
// stays a generic VM instead of growing a package manager in C.

// Pre-flight, BEFORE any image I/O: build/run/test need a project root, found
// by walking upward for a package.st. `st new` does not, because it is what
// creates one.
static _Bool planProject(CliArgs *cliArgs, ProjectPlan *plan)
{
	const char *subcommand = cliArgs->subcommand;
	if (strcmp(subcommand, "build") != 0 && strcmp(subcommand, "run") != 0
			&& strcmp(subcommand, "test") != 0) {
		return 1;
	}
	if (!projectFindRoot(plan->root, sizeof plan->root)) {
		fprintf(stderr, "st %s: no %s found from the current directory upward\n",
			subcommand, PROJECT_MANIFEST);
		return 0;
	}
	plan->hasProject = 1;
	projectBuildPath(plan->image, sizeof plan->image, plan->root, PROJECT_IMAGE);
	plan->force = cliArgs->force;
	plan->stale = plan->force || projectIsStale(plan->root, cliArgs->snapshotFileName);
	return 1;
}


// st test: the IMAGE decides which files run. ProjectTool prepareTests answers
// them newline-joined and points the session default namespace at <Root>Tests;
// each one then goes through the exact -f path, so a TestRun file works
// unchanged inside a project. The exit code is the summed failure count.
static int runProjectTests(void)
{
	static char paths[65536];
	if (!evalToCString("ProjectTool prepareTests", paths, sizeof paths)) {
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
			fflush(stdout); // the Smalltalk below writes unbuffered
			total += runFile(cursor);
		}
		if (newline == NULL) {
			break;
		}
		cursor = newline + 1;
	}
	return total;
}


// ---- the image --------------------------------------------------------------

static int writeImage(const char *path)
{
	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		fprintf(stderr, "st: cannot write the image '%s'\n", path);
		return 4;
	}
	int status = snapshotWrite(file);
	if (fclose(file) != 0 || status != 0) {
		fprintf(stderr, "st: writing the image '%s' failed\n", path);
		return 4;
	}
	return 0;
}


// Answers 1 when the image was loaded, 0 when it was passed over, and exits on
// a failure that leaves the heap unusable.
//
// The three outcomes are deliberately different. An image the caller NAMED with
// -s and that this build cannot read is REFUSED, because ignoring it would run
// the built-in kernel while the caller believed their image was loaded. One the
// SEARCH merely found is passed over with a note: every checkout of this project
// has a stale `snapshot` next to it that nobody asked for. And a file that
// passed the header check and then ran out is neither: the heap has already been
// replaced by then, so there is nothing left to fall back to.
static int readImage(const char *path, _Bool explicitlyNamed)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		return 0; // no image there: what every run looks like before one is made
	}
	char err[256];
	if (snapshotCheckHeader(file, err, sizeof err) != 0) {
		fclose(file);
		if (explicitlyNamed) {
			fprintf(stderr, "st: %s\n", err);
			exit(3);
		}
		fprintf(stderr, "st: ignoring the image at '%s' and running the built-in "
			"kernel (%s)\n", path, err);
		return 0;
	}
	rewind(file);
	int status = snapshotRead(file);
	fclose(file);
	if (status != 0) {
		fprintf(stderr, "st: the image '%s' could not be loaded\n", path);
		exit(3);
	}
	return 1;
}


// Run one parsed body with nil as the receiver, answering what it returned.
//
// A METHOD and not something special: the front end compiles methods, the tier
// compiles CodeUnits, and a body with no arguments is what both an expression on
// the command line and a top-level block in a file already are. Anything else
// would be a second path through the compiler that nothing else exercises.
static Value runBody(BlockNode *body, const char *what, int *failed)
{
	MethodNode *node = newObject(&Handles.MethodNode, 0);
	methodNodeSetSelector(node, stringFromC("doIt"));
	methodNodeSetBody(node, body);

	// No owner class: a top-level body has no instance variables to resolve
	// against, and every name in it is either its own temporary or a global.
	CompileContext context = { NULL, smalltalkGlobals(), NULL };
	CompileError error;
	CodeUnit *unit = compileMethod(node, &context, &error);
	if (unit == NULL) {
		fprintf(stderr, "st: %s: %s", what, compileStatusName(error.status));
		if (error.what != NULL) {
			fprintf(stderr, ": ");
			fprintRawString(stderr, error.what->raw);
		}
		fprintf(stderr, "\n");
		*failed = 1;
		return tagPtr(Handles.nil.raw);
	}

	Opcode unsupported = OP_COUNT;
	NativeCode *code = jitCompile(unit, &unsupported);
	if (code == NULL) {
		fprintf(stderr, "st: %s: the tier does not implement %s yet\n", what,
			opcodeName(unsupported));
		*failed = 1;
		return tagPtr(Handles.nil.raw);
	}
	return jitCall0(code, tagPtr(Handles.nil.raw));
}


// -f: a source file, run top to bottom.
//
// A file holds two kinds of thing and they are told apart by ONE token. `[`
// opens a top-level block, which is evaluated; anything else begins a class
// definition, which is built. That is the whole grammar of a file in this
// dialect, and it is what every file in tests/ is made of: a run of blocks, the
// last of which answers the failure count.
//
// THE LAST VALUE ANSWERED BECOMES THE EXIT CODE when it is an integer. The test
// suite reads it that way -- a file ends with `[ ^SomeRun report ]` and a
// nonzero count has to make the process fail, or every assertion-style test in
// the suite can pass while its checks do not.
static int runFile(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "st: cannot read '%s'\n", path);
		return 1;
	}

	HandleScope scope;
	openHandleScope(&scope);
	Parser parser;
	initFileParser(&parser, file, stringFromC(path));

	int failed = 0;
	int exitCode = 0;
	while (!parserAtEnd(&parser) && !failed) {
		HandleScope each;
		openHandleScope(&each);
		if (currentToken(&parser.tokenizer)->type == TOKEN_OPEN_SQUARE_BRACKET) {
			BlockNode *body = parseBlock(&parser);
			if (body == NULL) {
				printParseError(&parser, (char *) path);
				failed = 1;
			} else {
				Value answer = runBody(body, path, &failed);
				if (valueTypeOf(answer, VALUE_INT)) {
					exitCode = (int) asCInt(answer);
				}
			}
		} else {
			ClassNode *node = parseClass(&parser);
			if (node == NULL) {
				printParseError(&parser, (char *) path);
				failed = 1;
			} else {
				ClassBuildError error;
				classBuild(node, &error);
				if (error.message != NULL) {
					fprintf(stderr, "st: %s: %s", path, error.message);
					if (error.what != NULL) {
						fprintf(stderr, " '");
						fprintRawString(stderr, error.what->raw);
						fprintf(stderr, "'");
					}
					fprintf(stderr, "\n");
					failed = 1;
				}
			}
		}
		closeHandleScope(&each, NULL);
	}

	closeHandleScope(&scope, NULL);
	freeParser(&parser);
	fclose(file);
	return failed ? 1 : exitCode;
}


// Compile `source` as the body of a method, run it with nil as the receiver, and
// answer what it returned. 0 when it could not be compiled or run.
//
// A method and not something special: the front end compiles methods, the tier
// compiles CodeUnits, and an expression typed at the command line is a body with
// no arguments. Making it anything else would be a second path through the
// compiler that nothing else exercises.
//
// The ANSWER is what the project subcommands are built on: `ProjectTool build`
// reports the image path it wants written, and `ProjectTool run` reports the
// program's exit code. Discarding it, which is all -e ever needed, is the
// special case rather than the general one.
static _Bool evalValue(const char *source, const char *what, Value *answer)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t length = strlen(source) + 32;
	char *wrapped = malloc(length);
	if (wrapped == NULL) {
		closeHandleScope(&scope, NULL);
		return 0;
	}
	snprintf(wrapped, length, "doIt [ %s ]", source);

	Parser parser;
	initParser(&parser, stringFromC(wrapped));
	MethodNode *node = parseMethod(&parser);
	if (node == NULL) {
		printParseError(&parser, (char *) what);
		freeParser(&parser);
		free(wrapped);
		closeHandleScope(&scope, NULL);
		return 0;
	}

	// No owner class: an expression has no instance variables to resolve
	// against, and every name in it is either its own temporary or a global.
	// The third field is spelled out rather than left to the implicit zero: it is
	// classVariableScope, whose NULL means "same as ownerClass", and it is the
	// field whose absence was silent the last time it was got wrong.
	CompileContext context = { NULL, smalltalkGlobals(), NULL };
	CompileError error;
	CodeUnit *unit = compileMethod(node, &context, &error);
	freeParser(&parser);
	free(wrapped);
	if (unit == NULL) {
		fprintf(stderr, "st: %s", compileStatusName(error.status));
		if (error.what != NULL) {
			fprintf(stderr, ": ");
			fprintRawString(stderr, error.what->raw);
		}
		fprintf(stderr, "\n");
		closeHandleScope(&scope, NULL);
		return 0;
	}

	Opcode unsupported = OP_COUNT;
	NativeCode *code = jitCompile(unit, &unsupported);
	if (code == NULL) {
		fprintf(stderr, "st: the tier does not implement %s yet\n",
			opcodeName(unsupported));
		closeHandleScope(&scope, NULL);
		return 0;
	}
	Value result = jitCall0(code, tagPtr(Handles.nil.raw));
	closeHandleScope(&scope, NULL);
	// Read out with NOTHING allocating in between: the scope is closed, so a
	// pointer answer is only valid until the next allocation, and every caller
	// either copies the bytes immediately or reads an immediate.
	if (answer != NULL) {
		*answer = result;
	}
	return 1;
}


static int evaluate(const char *source)
{
	return evalValue(source, "-e", NULL) ? 0 : 1;
}


// Evaluate `code` and copy its String answer into `buffer`.
//
// The bytes are copied BEFORE anything else allocates, which is the whole
// reason this is a function and not two lines at each call site: the String is
// a heap object, and the next allocation may move it.
static _Bool evalToCString(const char *code, char *buffer, size_t size)
{
	Value value;
	if (!evalValue(code, code, &value) || !valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return 0; // nil, which is how ProjectTool reports a failure it printed
	}
	size_t length = rawObjectElementCount(object);
	if (length + 1 > size) {
		return 0;
	}
	memcpy(buffer, rawObjectBytes(object), length);
	buffer[length] = '\0';
	return 1;
}


// Evaluate `code` expecting an integer exit code; anything else is a failure.
static int evalToInt(const char *code)
{
	Value value;
	if (!evalValue(code, code, &value)) {
		return EXIT_FAILURE;
	}
	return valueTypeOf(value, VALUE_INT) ? (int) asCInt(value) : EXIT_FAILURE;
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

	ProjectPlan plan = { 0, 0, 0, { 0 }, { 0 } };

	initThread(&CurrentThread);
	initHandles();
	bootstrapBuiltinKernel();

	// The project subcommands.
	//
	// The ORDER here is the design. Root discovery and the staleness decision
	// happen before any image is touched, so `st build` on an up-to-date project
	// never boots a heap at all; then the right image is loaded (the built one
	// when it is fresh, the base one when a build is about to happen); then the
	// build, if any; then the command itself.
	if (cliArgs.subcommand != NULL) {
		const char *name = cliArgs.subcommand;
		if (strcmp(name, "repl") == 0) {
			return notPortedYet("st repl", "the read-eval-print loop");
		}
		if (!planProject(&cliArgs, &plan)) {
			return EXIT_FAILURE;
		}
		if (strcmp(name, "build") == 0 && !plan.stale) {
			printf("up to date\n");
			return EXIT_SUCCESS;
		}

		// A fresh project image already holds the package graph, so it is loaded
		// INSTEAD of the base image; a stale one means the base image is loaded
		// and the graph is built onto it below.
		const char *image = plan.hasProject && !plan.stale
			? plan.image : cliArgs.snapshotFileName;
		if (image != NULL) {
			readImage(image, cliArgs.snapshotExplicit || image == plan.image);
		}

		if (plan.hasProject && plan.stale) {
			char out[PROJECT_PATH_MAX];
			// ProjectTool answers the path it wants the image written to, or nil
			// after printing its own error. nil is not a String, so evalToCString
			// answers 0 and the message the image already printed is the whole
			// report; adding one here would say it twice.
			if (!evalToCString("ProjectTool build", out, sizeof out)) {
				return EXIT_FAILURE;
			}
			// COLLECT FIRST. The build allocated a package loader, a parse tree per
			// file and every transient behind them, and none of it is reachable
			// now; writing without collecting puts all of it in the image, where it
			// is dead weight that also has to be read back on every load.
			collectGarbage(CurrentThread.heap);
			int status = writeImage(out);
			if (status != 0) {
				return status;
			}
			printf("built %s\n", out);
			fflush(stdout); // the program's own output below is unbuffered
			if (strcmp(name, "build") == 0) {
				return EXIT_SUCCESS;
			}
		}

		if (strcmp(name, "new") == 0) {
			return evalToInt("ProjectTool scaffold");
		}
		if (strcmp(name, "run") == 0) {
			return evalToInt("ProjectTool run");
		}
		if (strcmp(name, "test") == 0) {
			return runProjectTests();
		}
		fprintf(stderr, "st: unknown subcommand '%s'\n", name);
		return EXIT_FAILURE;
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
		// SAVING NEEDS -s, and only -s. `-b DIR` on its own builds the classes and
		// reports, which is what makes `-b DIR -e CODE` the cheapest way to find
		// out what the real kernel does when it executes; writing to the resolved
		// default would have it clobber whatever image is next to the checkout
		// without anyone asking.
		if (cliArgs.snapshotExplicit) {
			int status = writeImage(cliArgs.snapshotFileName);
			if (status != 0) {
				return status;
			}
		}
		// The image is written BEFORE the expression runs, so it holds the package
		// as built and not as some probe left it.
		if (cliArgs.eval != NULL) {
			return evaluate(cliArgs.eval);
		}
		if (!cliArgs.snapshotExplicit) {
			fprintf(stderr, "st: nothing was saved; name an image with -s to write "
				"one\n");
		}
		return 0;
	}

	if (cliArgs.snapshotFileName != NULL) {
		readImage(cliArgs.snapshotFileName, cliArgs.snapshotExplicit);
	}

	// ST_RESAVE: load an image and write it straight back out. It exists for
	// scripts/check-image-idempotence.sh, which is the only check that can catch
	// a field the writer persists and the reader drops -- the image still loads
	// and still runs, and the difference shows up nowhere else.
	const char *resave = getenv("ST_RESAVE");
	if (resave != NULL) {
		int status = writeImage(resave);
		if (status != 0) {
			return status;
		}
	}

	if (cliArgs.fileName != NULL) {
		return runFile(cliArgs.fileName);
	}
	if (cliArgs.eval != NULL) {
		return evaluate(cliArgs.eval);
	}
	printCliHelp();
	return 0;
}
