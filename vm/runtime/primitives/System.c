// The process the VM is running in: its environment, and what it was told.
//
// Everything here goes through the OS seam (os/Os.h); nothing calls libc
// directly, for the same reason the filesystem domain does not.

#include "runtime/primitives/Shared.h"
#include "runtime/String.h"
#include "jit/Jit.h"
#include "os/Os.h"
#include "runtime/Collection.h"
#include <stdio.h>
#include <stdlib.h>

// Longer than any environment value worth putting in one, and the same bound
// the project tooling uses for a path list.
#define ENVIRONMENT_VALUE_MAX 8192


// System class primitiveGetEnv: aString
//
// FAILS when the variable is unset, and the kernel's `^nil` fallback is the
// answer for that. Failing rather than answering nil keeps "unset" and "set to
// the empty string" distinguishable, which is what `System env:ifAbsent:` is
// written to tell apart.
Value primGetEnv(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value nameValue = primitiveArgument(args, 0);
	if (!valueTypeOf(nameValue, VALUE_POINTER)
			|| rawObjectFormat(asObject(nameValue)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	RawObject *nameObject = asObject(nameValue);
	size_t nameLength = rawObjectElementCount(nameObject);
	char name[256];
	if (nameLength + 1 > sizeof name) {
		return PRIMITIVE_FAILED;
	}
	memcpy(name, rawObjectBytes(nameObject), nameLength);
	name[nameLength] = '\0';

	// Read into C storage BEFORE allocating: building the answer String can move
	// the name argument, and getenv's own memory is not stable across a setenv.
	char value[ENVIRONMENT_VALUE_MAX];
	if (!osEnvironmentValue(name, value, sizeof value)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(value));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// What the process was told, and what it is running on
// ---------------------------------------------------------------------------
//
// All of these are one call each into os/Os.h, which already had every one of
// them: the executable path, the core count and the entropy source were written
// for the old VM and never removed. What was missing was the primitives, which
// is why `System arguments` answered `primitiveFailed:` and took `st new`,
// `st run` and every project e2e down with it.
//
// THE COMMAND LINE IS THE EXCEPTION, because no OS call answers it: it arrives
// in main's argv and nothing else in the process has it. tools/Cli.h owns the
// split between the VM's own flags and the program's arguments, and it is
// PARSED THERE and handed here, so this file does not acquire a second opinion
// about what `-e` means.

static int gArgumentCount;
static char **gArguments;


void primitiveSetCommandLine(int count, char **arguments)
{
	gArgumentCount = count;
	gArguments = arguments;
}


// System class primitiveCommandLine
//
// An OrderedCollection of Strings, empty when none were passed, which is what
// the kernel documents. Empty rather than nil: `System arguments do: [...]` is
// the ordinary use and it must not have to test first.
// System class flushSendCaches
//
// The escape hatch its own comment in packages/Core/src/System.st promises, and
// it went from decorative to load-bearing when send sites grew an inline cache:
// before that a cached target was read only by the runtime that had just written
// it, and now COMPILED CODE reads it, so a reflective edit to a method
// dictionary that nothing else notices leaves armed sites calling the method
// that used to be there.
//
// The routine mutations already invalidate on their own -- installing a method
// (tools/ClassBuilder.c) and removing one (primClassRemoveSelector) both call
// this same function -- so what is left for this primitive is exactly what the
// kernel says it is: surgery done some other way.
//
// NO STOP-THE-WORLD BRACKET, and the kernel comment naming one describes where
// this is going rather than where it is: the v2 scheduler runs a single OS
// thread, so there is no peer to stop. When workers return this needs the
// safepoint bracket, because clearing a target under a peer that is mid-probe is
// the torn read the cache design already warns about (jit/InlineCache.h).
Value primFlushSendCaches(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	jitFlushSendCaches();
	return primitiveReceiver(args);
}


// VMTools class backtrace
//
// The compiled frames under this send, printed newest first. It answers the
// receiver so it can be dropped into a cascade, and it prints on stderr for the
// reason every diagnostic here does: stdout is what a test's own output is
// compared on, and a backtrace must not become part of it.
Value primPrintBacktrace(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	jitPrintBacktrace(stderr);
	return primitiveReceiver(args);
}


Value primCommandLine(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *arguments = newOrdColl(gArgumentCount == 0 ? 1
		: (size_t) gArgumentCount);
	for (int i = 0; i < gArgumentCount; i++) {
		// Each String is added IMMEDIATELY after it is built, so nothing holds a
		// bare Value across the next allocation.
		ordCollAddObject(arguments, (Object *) stringFromC(gArguments[i]));
	}
	Value answer = objectTagged(arguments);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// System class primitiveCpuCount
Value primCpuCount(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	(void) args;
	int count = osAvailableCoreCount();
	return tagInt(count < 1 ? 1 : count);
}


// System class executablePath
Value primExecutablePath(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	char path[4096];
	if (!osExecutablePath(path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(path));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// System class primitiveExit: anInteger
//
// THE PROCESS ENDS HERE and this never answers. It is deliberately NOT the
// terminate path: `Processor thisProcess terminate` unwinds, running every
// pending `ensure:` on the way out, and `System exit:` says "immediately" --
// it is the Smalltalk spelling of _exit, for a program that has decided not to
// finish. A caller that wants the cleanups has the other one.
//
// The status is masked to eight bits for the same reason main's is: an exit
// status is eight bits, and a count that happened to be a multiple of 256 would
// otherwise report success.
Value primExitWithCode(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value code = primitiveArgument(args, 0);
	if (!valueTypeOf(code, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	fflush(NULL); // the kernel's streams are its own, but stdio may hold output
	exit((int) ((unsigned) asCInt(code) & 0xFFu));
}


// System class primitiveRandomBytes: aByteArray
//
// FILLS THE RECEIVER'S ARGUMENT IN PLACE, which is what the kernel's
// `randomBytes:` wrapper is built on: it allocates the ByteArray and hands it
// here, so this never allocates and the entropy call cannot be interrupted by a
// collection.
Value primRandomBytes(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value buffer = primitiveArgument(args, 0);
	if (!valueTypeOf(buffer, VALUE_POINTER)
			|| rawObjectFormat(asObject(buffer)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	RawObject *bytes = asObject(buffer);
	size_t size = rawObjectElementCount(bytes);
	if (size != 0 && !osRandomBytes(rawObjectBytes(bytes), size)) {
		return PRIMITIVE_FAILED;
	}
	return primitiveReceiver(args);
}


// DateTime class>>localUtcOffsetAt: epochSeconds
//
// Seconds east of UTC in effect AT THAT INSTANT, which is why it takes one: the
// offset is not a property of the zone but of the zone and the moment, and a
// DST boundary is exactly where a cached one is wrong.
Value primLocalUtcOffset(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value seconds = primitiveArgument(args, 0);
	if (!valueTypeOf(seconds, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) osLocalUtcOffsetSeconds((int64_t) asCInt(seconds)));
}
