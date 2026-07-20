#include "jit/PerfMap.h"
#include "core/CompiledCode.h"
#include "core/Object.h"
#include "core/Smalltalk.h"
#include "os/OsThread.h"
#include "os/OsFile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// perf-map emitter. The platform profiler cannot symbolize JIT code on its
// own: every compiled method shows up as an anonymous hex address, which is
// where ~98% of this VM's userspace cycles live. Where the platform has a map
// convention (osJitMapPath — Linux: /tmp/perf-<pid>.map for `perf`), we emit
// one line per compiled method/block at the single build funnel
// (buildNativeCode):
//   <hexStartAddr> <hexByteSize> <name>\n
// e.g.  7f2c00120480 1a0 String>>asLowercase
// The whole feature is gated on ST_PERF_MAP; when off it is one cached branch
// per COMPILE (never per execution), so steady-state req/s is untouched.

static int gPerfMapState = 0;   // 0 = unknown, 1 = disabled, 2 = enabled (fd open)
static OsFd gPerfMapFd = OS_FD_INVALID;
static OsMutex gPerfMapInitLock = OS_MUTEX_STATIC_INIT;


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
	char path[256];
	if (!osJitMapPath(path, sizeof(path))) {
		return 1; // this platform has no profiler map convention
	}
	// APPEND_TRUNC: start fresh (stale lines from a previous run reusing this
	// pid would mis-resolve), then every single write lands at end-of-file
	// atomically, which is the entire cross-thread and tier-recompile story
	// below.
	OsFd fd = osFileOpen(path, OS_FILE_APPEND_TRUNC);
	if (fd == OS_FD_INVALID) {
		return 1; // give up quietly; a profiling aid must never disturb the run
	}
	gPerfMapFd = fd;
	return 2;
}


static _Bool perfMapEnabled(void)
{
	int state = __atomic_load_n(&gPerfMapState, __ATOMIC_ACQUIRE);
	if (state == 0) {
		osMutexLock(&gPerfMapInitLock);
		state = __atomic_load_n(&gPerfMapState, __ATOMIC_RELAXED);
		if (state == 0) {
			state = perfMapOpen(); // sets gPerfMapFd before we publish the state
			__atomic_store_n(&gPerfMapState, state, __ATOMIC_RELEASE);
		}
		osMutexUnlock(&gPerfMapInitLock);
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

	// One line, one write. perf reads the remainder of the line after the size
	// as the symbol name, so ">>", spaces and "[]" are all fine. A single short
	// write in append mode is atomic against concurrent writers (other workers
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
	int64_t written = osFileWrite(gPerfMapFd, line, (size_t) len);
	(void) written; // best-effort tracing: a short write garbles at most one line
}
