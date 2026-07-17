Profiling and reading the generated JIT code
============================================

There is no interpreter in this VM: every method and block is compiled to
machine code before it runs. That is good for speed and bad for observability,
because a Linux profiler sees the running code as one big anonymous executable
mapping with no symbols. `perf report` on a busy server shows almost all of the
userspace cycles as bare hex addresses, and you cannot tell which Smalltalk
method is hot.

This guide covers how to get method-level attribution back (the `ST_PERF_MAP`
emitter), how to profile the VM under load, and how to read the actual machine
code the JIT produced for any named method.

Everything here is Linux and x86-64 unless noted. The commands were run against
`build/st` and the business-card HTTP sample.


The perf map
------------

`perf` has a standard escape hatch for JITs: if a file `/tmp/perf-<pid>.map`
exists, it reads it during `perf report` / `perf script` and resolves any
instruction pointer that falls in a JIT mapping to a name. Each line is

```
<hexStartAddr> <hexByteSize> <name>
```

for example

```
7f2c00120480 1a0 String>>asLowercase
```

Set `ST_PERF_MAP=1` and the VM writes one such line per compiled method and
block:

```sh
ST_PERF_MAP=1 LD_LIBRARY_PATH=build build/st -s snapshot -f server.st
# writes /tmp/perf-<pid>.map
```

Details worth knowing:

- The feature is gated. With `ST_PERF_MAP` unset, empty, or `0`, nothing is
  emitted and the cost is a single cached branch per compile (never per method
  call), so a production run pays nothing. The file is opened lazily on the
  first compile, so a run that never reaches codegen leaves no file behind.
- The name is `Class>>selector`. Class-side methods render as
  `Class class>>selector`, and blocks get a `[]` suffix, for example
  `SmallInteger>>printStringBase:[]`. Nested blocks share their method's label.
- The tier recompiles hot methods, so a method's code address changes over its
  lifetime. The map is append-only (`O_APPEND`), so a recompiled method simply
  gets a second line at its new address. `perf` tolerates the overlap and
  resolves each address to the same name. Executable space never moves or is
  freed, so every address in the map stays valid for the life of the process.
- Writes are a single `write()` per line under `O_APPEND`, which is atomic
  against the file offset, so concurrent worker threads compiling at the same
  time never interleave a line.
- Stubs (the shared IC probe, the allocation stub, and so on) are not named:
  they are not built through the method/block funnel. They show up as a small
  number of anonymous `[JIT]` addresses, which is expected.

The emitter lives in `vm/jit/PerfMap.c` and is called from `buildNativeCode`,
the single place both methods and blocks finish compilation.


Profiling under load
--------------------

`perf record` needs only user-space cycles here, which works at the default
`perf_event_paranoid=2`. Sample with `cycles:u` so no kernel access is needed.

A safe, repeatable recipe pins the server and the load generator to disjoint
cores so the desktop keeps some. On a 16-core box, server on 0-7, load on 12-15,
and 8-11 left free:

```sh
# 1. start the server with the map enabled, pinned to cores 0-7
taskset -c 0-7 env ST_PERF_MAP=1 ST_SCHED_WORKERS=8 \
  LD_LIBRARY_PATH=build build/st -s snapshot -f server.st &
PID=$!

# 2. warm it up so the tier has recompiled the hot handlers
taskset -c 12-15 ab -k -c 40 -n 200000 http://127.0.0.1:8091/health

# 3. profile while a bounded load runs
taskset -c 12-15 ab -k -c 40 -n 1500000 http://127.0.0.1:8091/health &
perf record -e cycles:u -g -p "$PID" -- sleep 8
```

Then read it two ways. Inclusive (who is on the stack, top of the tree is the
accept loop):

```sh
perf report --stdio -g none        # Children + Self columns
```

Self time (where cycles actually burn, the useful view for finding a hot leaf):

```sh
perf report --stdio --no-children -g none
```

A real run against `/health` of the business-card sample put the top self-time
in `CollectionStream>>nextPutAll:`, `HttpConnection>>readLine[]`,
`Json class>>encode:on:depth:`, and the integer formatting path
(`Integer>>floorLog:`, `Integer>>//`) used by the JSON number encoder, next to
genuine C symbols like `allocateObject` and `pthread_mutex_lock` from
`libVM.so`. All of that used to be anonymous hex.

Notes:

- Warm up before recording. The tier recompiles handlers on the fly, and you do
  not want the recompile churn inside your sample window.
- Do not tear the server down while `perf` is attached and load is in flight.
  The abrupt-kill path can crash the shutdown; let the load finish, detach
  `perf`, then stop the server. This is a property of the server teardown, not
  of the profiler or the map.
- On loopback, a single `ab` process saturates around 60k req/s per core, so to
  measure a server ceiling above that, run several `ab` instances or use an
  external client.


Reading the machine code of a method
------------------------------------

The map does not only name frames. It gives you the one thing that used to be
missing: the address and size of a named method. With that you can disassemble
the exact code the JIT produced for it.

Pick the line you want from the map (take the last occurrence of a name to get
the most recent tier-1 code):

```sh
grep -F ' String>>headerNameMatches:' /tmp/perf-$PID.map | tail -1
# 7f069bac40a8 1c5a String>>headerNameMatches:
```

Live-disassemble that range by attaching gdb to the running process:

```sh
gdb -p "$PID" -batch \
  -ex 'set pagination off' \
  -ex 'disassemble 0x7f069bac40a8, 0x7f069bac40a8+0x1c5a'
```

The first instructions are the standard framed prologue the backend emits
(`generatePrologue` / `generateContextDefinition`):

```
push   %rbp
mov    %rsp,%rbp
sub    $0x90,%rsp
movabs $0x7f06a1fb0821,%r10      ; nil
mov    %r10,-0x8(%rbp)           ; nil-init each frame slot
mov    %r10,-0x10(%rbp)
...
```

If you prefer an offline listing, dump the bytes and run objdump. Intel syntax
here, drop `-M intel` for AT&T:

```sh
gdb -p "$PID" -batch -ex 'set pagination off' \
  -ex 'dump binary memory method.bin 0x7f069bac40a8 0x7f069bac40a8+0x1c5a'
objdump -D -b binary -m i386:x86-64 -M intel method.bin
```

`perf annotate` does not help for JIT map symbols: the `.map` carries names
only, no code bytes, so `perf` has nothing to disassemble and reports no
samples. Use the gdb or objdump path above to read JIT machine code.

Requirements: gdb needs to be able to attach (`ptrace_scope` 0 or 1 and same
uid; the server started as a child of your shell satisfies scope 1).


JIT knobs for A/B and isolation
-------------------------------

None of these change correctness; they are for comparing configurations and for
narrowing down what a change actually moved. All are environment variables read
once at startup.

| Variable | Effect |
| --- | --- |
| `ST_PERF_MAP=1` | Emit `/tmp/perf-<pid>.map` (this document). |
| `ST_NO_IC=1` | Disable inline caches. Every send goes through the full lookup. Also disables the tier. |
| `ST_NO_TIER=1` | Keep inline caches, disable adaptive recompilation. Everything stays tier-0. |
| `ST_TIER_THRESHOLD=<n>` | Invocations before a method recompiles at tier 1. Default 1000. Lower it to force recompiles quickly when testing. |
| `ST_TIER_INLINE_MAX=<n>` | Max callee size for speculative inlining. Default 24. `0` disables inlining but keeps promotion to direct calls (the tier-1 M1 shape). |
| `ST_TIER_STATS=1` | Print tier counters at exit (recompiles, promoted sites, inlined sites, guard fails). |
| `ST_IC_STATS=1` | Print inline-cache counters at exit (hits, PIC builds, mega promotes). |
| `ST_NO_INLINE_CF=1` | Do not inline control flow (`ifTrue:`, `whileTrue:`, and friends). Every one becomes a real block send. |
| `ST_NO_INLINE_FLOAT=1` | Do not use the float call-site intrinsic. Float arithmetic goes through normal sends. |
| `ST_JIT_REGS=<n>` | Shrink the allocatable register pool to `n`. Test-only, for exercising the spiller. |
| `ST_SCHED_WORKERS=<n>` | Number of OS-thread workers running the fiber scheduler. |

A typical comparison is to run the same workload under the default, then under
`ST_NO_TIER=1`, then `ST_NO_IC=1`, and watch `perf stat -e cycles:u,
instructions:u` to separate "executes fewer instructions" from "same
instructions, better frontend". `perf stat` was the tool that showed the
multicore ceiling is cache-coherence stall, not lock contention, on this VM.


Where the pieces are
--------------------

- `vm/jit/PerfMap.c`, `vm/jit/PerfMap.h`: the map emitter.
- `vm/jit/x64/CodeGeneratorX64.c`, `vm/jit/ppc64/CodeGeneratorPpc64.c`:
  `buildNativeCode` is the funnel that calls `perfMapEmit`.
- `vm/core/StackFrame.c` `printBacktrace`: the same `NativeCode` to
  `Class>>selector` resolution, for backtraces.
- `vm/core/CompiledCode.h`: the `NativeCode` layout (`insts` is the code start,
  `size` is the byte length, `compiledCode` points back to the method or block).
