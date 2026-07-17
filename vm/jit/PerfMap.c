#include "jit/PerfMap.h"
#include "core/CompiledCode.h"
#include "core/Object.h"
#include "core/Smalltalk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>

// perf-map emitter. Linux `perf` cannot symbolize JIT code on its own: every
// compiled method shows up as an anonymous hex address, which is where ~98% of
// this VM's userspace cycles live. If /tmp/perf-<pid>.map exists, perf reads it
// during `perf report` and resolves each JIT frame to a name. Each line is:
//   <hexStartAddr> <hexByteSize> <name>\n
// e.g.  7f2c00120480 1a0 String>>asLowercase
// We emit one line per compiled method/block at the single build funnel
// (buildNativeCode). The whole feature is gated on ST_PERF_MAP; when off it is
// one cached branch per COMPILE (never per execution), so steady-state req/s is
// untouched.

static int gPerfMapState = 0;   // 0 = unknown, 1 = disabled, 2 = enabled (fd open)
static int gPerfMapFd = -1;
static pthread_mutex_t gPerfMapInitLock = PTHREAD_MUTEX_INITIALIZER;


// Open the map exactly once, lazily on the first compile, so a run that never
// reaches codegen (a C self-test that exits early) leaves no file behind. The
// gPerfMapFd store happens-before the RELEASE of gPerfMapState in the caller,
// so any thread that observes state == 2 also sees the valid fd. Returns the
// state to publish (1 disabled, 2 enabled).
static int perfMapOpen(void)
{
	const char *flag = getenv("ST_PERF_MAP");
	if (flag == NULL || flag[0] == '\0' || (flag[0] == '0' && flag[1] == '\0')) {
		return 1; // absent, empty, or "0": stay disabled
	}
	char path[64];
	snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int) getpid());
	// O_TRUNC: start fresh (stale lines from a previous run reusing this pid
	// would mis-resolve). O_APPEND: from here on every single write() lands at
	// end-of-file atomically, which is the entire cross-thread and
	// tier-recompile story below.
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644);
	if (fd < 0) {
		return 1; // give up quietly; a profiling aid must never disturb the run
	}
	gPerfMapFd = fd;
	return 2;
}


static _Bool perfMapEnabled(void)
{
	int state = __atomic_load_n(&gPerfMapState, __ATOMIC_ACQUIRE);
	if (state == 0) {
		pthread_mutex_lock(&gPerfMapInitLock);
		state = __atomic_load_n(&gPerfMapState, __ATOMIC_RELAXED);
		if (state == 0) {
			state = perfMapOpen(); // sets gPerfMapFd before we publish the state
			__atomic_store_n(&gPerfMapState, state, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&gPerfMapInitLock);
	}
	return state == 2;
}


void perfMapEmit(struct NativeCode *code)
{
	if (!perfMapEnabled()) {
		return;
	}

	// Resolve the back-pointer exactly as printBacktrace does (StackFrame.c): a
	// block's compiledCode is a CompiledBlock whose owning method carries the
	// selector; a class-side method's ownerClass is a MetaClass to unwrap to its
	// instance class. No allocation and no handles: buildNativeCode holds
	// codegenLock so nothing moves, and every object read here is live.
	RawCompiledMethod *method = (RawCompiledMethod *) code->compiledCode;
	if (method == NULL) {
		return; // stubs / degenerate code carry no method identity
	}
	const char *blockSuffix = "";
	if (method->class == Handles.CompiledBlock->raw) {
		RawCompiledBlock *block = (RawCompiledBlock *) method;
		method = (RawCompiledMethod *) asObject(block->method);
		blockSuffix = "[]";
	}
	RawClass *class = (RawClass *) asObject(method->ownerClass);
	const char *classSuffix = "";
	if (class->class == Handles.MetaClass->raw) {
		class = (RawClass *) asObject(((RawMetaClass *) class)->instanceClass);
		classSuffix = " class";
	}
	RawString *className = (RawString *) asObject(class->name);
	RawString *selector = (RawString *) asObject(method->selector);

	// One line, one write(). perf reads the remainder of the line after the size
	// as the symbol name, so ">>", spaces and "[]" are all fine. A single short
	// write() under O_APPEND is atomic against concurrent writers (other workers
	// compiling in other heaps) and against the file offset, so no separate lock
	// is needed. Truncation (name longer than the buffer) only shortens a label.
	char line[512];
	int len = snprintf(line, sizeof(line),
		"%lx %lx %.*s%s>>%.*s%s\n",
		(unsigned long) (uintptr_t) code->insts,
		(unsigned long) code->size,
		(int) className->size, className->contents, classSuffix,
		(int) selector->size, selector->contents, blockSuffix);
	if (len <= 0) {
		return;
	}
	if ((size_t) len > sizeof(line)) {
		len = (int) sizeof(line); // snprintf return is the untruncated length
	}
	ssize_t written = write(gPerfMapFd, line, (size_t) len);
	(void) written; // best-effort tracing: a short write garbles at most one line
}
