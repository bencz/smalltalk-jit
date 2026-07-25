#ifndef PERF_MAP_H
#define PERF_MAP_H

struct NativeCode;

// Emit one /tmp/perf-<pid>.map line for a freshly built method or block native
// code, so Linux `perf report` resolves the otherwise-anonymous JIT frames back
// to "Class>>selector". A no-op (a single cached branch) unless ST_PERF_MAP is
// set, so a production run pays nothing. Called from buildNativeCode, the single
// method+block compilation funnel, which always runs under codegenLock. Safe to
// call with code->compiledCode == NULL (it just returns). See PerfMap.c.
void perfMapEmit(struct NativeCode *code);

// Same, for code that carries no method identity and therefore cannot be named
// by perfMapEmit: the shared stubs (allocate, lookup, DNU, PIC probe, entry).
// Without this every stub sample lands in the profiler's unresolved-address
// bucket, which is exactly the bucket you are trying to explain when you ask
// "how much of this run is allocation?". `name` is copied into the line as-is.
void perfMapEmitNamed(struct NativeCode *code, const char *name);

#endif
