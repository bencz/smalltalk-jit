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

#endif
