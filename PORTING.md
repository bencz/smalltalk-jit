# Porting the VM to a new CPU or OS

The tree is organized so that CPU- and OS-specific code lives in exactly two
places, bound at **link time** (CMake decides which files to compile; generic
code calls through neutral contract headers). There is deliberately no
`#ifdef` forest: the only architecture `#if` in the tree is the dispatch chain
in `vm/jit/TargetAssembler.h`, plus an `#error` guard at the top of each
backend source.

```
vm/jit/<arch>/   ALL CPU-specific code (assembler, codegen, stubs,
                 primitive generators, fiber switch)
vm/os/<os>/      ALL OS-specific code behind the vm/os/Os.h contract,
                 split by domain (OsTime, OsMemory, OsEvents, OsSignals,
                 OsCpu, ... one file per domain)
```

## Adding a CPU backend (aarch64, riscv64, ppc64le, ppc64)

Create `vm/jit/<arch>/` providing:

1. **`Assembler<Arch>.h`** — the ISA model: a `Register` enum, `TMP`/`CTX`
   register defines, `Registers[]`/`AvailableRegs` allocation pool,
   `ArgumentsRegisters[]`/callee-saved tables for the C ABI, the instruction
   emitters the code generator uses, `TARGET_CODE_FILLER_BYTE` (a breakpoint/
   illegal pattern so a stray jump into unwritten code traps), and a
   TP-relative TLS load (`asmLoadTls` equivalent — see *TLS* below).
2. **`CodeGenerator<Arch>.c`** — implements the generation API declared in
   `vm/jit/CodeGenerator.h` (method/block codegen, sends + inline caches,
   write barrier, safepoint polls, C-call trampolines, stackmap emission).
3. **`StubCode<Arch>.c`** — the four stubs in `vm/jit/StubCode.h`:
   Smalltalk entry (C→JIT trampoline saving callee-saved regs into an
   `EntryStackFrame`), TLAB allocate, lookup, doesNotUnderstand.
4. **`Primitives<Arch>.c`** — every generator listed in
   `vm/jit/PrimitivesGen.def` plus `generateCCallPrimitive` (contract header:
   `vm/jit/TargetPrimitives.h`). Bring-up shortcut: stub them all with
   `#define GEN_PRIMITIVE(name, fn) void fn(CodeGenerator *g) { FAIL(); }` /
   `#include "jit/PrimitivesGen.def"` and implement incrementally.
5. **`Fiber<Arch>.c`** — `fiberSwitchAsm` (callee-saved swap) and
   `fiberTargetPrimeStack` (initial stack frame + entry alignment); contract:
   `vm/jit/TargetFiber.h`.

Register the arch in the **two** registration points (each fails loudly if
they disagree):
- the `ST_ARCH` block in `CMakeLists.txt` (adds the backend's `.c` files),
- the dispatch chain in `vm/jit/TargetAssembler.h`.

`grep -rn 'PORT_ME'` lists every known hazard in generic code. The big ones:

| Topic | Where | What a port must do |
|---|---|---|
| `PORT_ME(tls)` | `vm/core/Thread.c` | `gCurrentThreadTpoff`/`gLookupCacheTpoff` are byte offsets from the thread pointer, computed portably via `__builtin_thread_pointer()`. The JIT bakes them into shared code and loads TP-relative per worker (x64: `%fs`). aarch64 uses `tpidr_el0`, riscv uses `tp`; **ppc64 uses r13 with a 0x7000 bias in the TLS ABI** — re-derive the offset semantics per ABI before trusting the subtraction. |
| `PORT_ME(icache)` | `vm/os/Os.h` | `osFlushICache` is already called at every code-publish point (the `buildNativeCodeFromAssembler` funnel + the scavenger's baked-immediate patch loop). `__builtin___clear_cache` handles the local hart, **but RISC-V `fence.i` is hart-local**: cross-thread publication of fresh code needs a per-arch audit (remote fence via syscall, or publish handshakes). |
| `PORT_ME(wxorx)` | `vm/memory/HeapPage.c` | Executable pages are RWX for the process lifetime. Hardened kernels (and some platforms) refuse RWX: add a W^X toggle protocol around code writes. |
| `PORT_ME(endianness)` | `vm/tools/Snapshot.c` | The image is a raw dump of native-endian words (object headers, `InstanceShape` reinterpreted as `int64`, bytecode streams with native-endian `int32` jump offsets). **Little-endian only today**; ppc64 BE needs a byte-order-explicit format (or a BE-built image with no cross-endian exchange). |
| `PORT_ME(memory-model)` | `vm/memory/Safepoint.h` | All cross-thread protocols use `__atomic` acquire/release builtins — portable in principle, but validated only under x86-TSO. Audit every publish/observe pair (safepoint flags, JIT code publish, remembered sets) before the first weak-memory port. |
| `PORT_ME(frame-layout)` | `vm/core/StackFrame.c` | The frame walker's slot arithmetic mirrors the frame the JIT emits. A backend must reproduce that layout (saved BP at `*frame`, slots growing down) or take over these accessors. |
| `PORT_ME(addr-tagging)` | `vm/core/Object.h` | Bit 3 of an object's *address* distinguishes young/old space. Keep 16-byte object alignment and page mappings that leave that bit meaningful. Arch-neutral, but an invariant to respect. |
| 64-bit only | `vm/core/Object.h` | `_Static_assert(sizeof(Value) == 8)`: tagged 62-bit ints and the descriptor bit-packing assume 64-bit `Value`. 32-bit targets are out of scope. |

## Adding an OS (windows, osx, aix, ...)

Create `vm/os/<os>/` implementing every function in `vm/os/Os.h`, one file per
domain (mirror `vm/os/linux/`):

| File | Contract slice | Linux implementation |
|---|---|---|
| `OsTime.c` | `osCurrentMicroTime` | `gettimeofday` |
| `OsMemory.c` | page alloc/reserve/commit/release/free, `osPageSize`, `osFlushICache` | `mmap`/`mprotect`/`madvise` |
| `OsEvents.c` | the opaque `OsEventLoop` (arm/disarm/wait/wake) | epoll + wake-eventfd (kqueue/IOCP would slot in) |
| `OsSignals.c` | `osInstallSegvHandler` (only the fault ADDRESS is consumed — no mcontext poking), `osIgnoreBrokenPipe` | `sigaction` + per-thread `sigaltstack` |
| `OsCpu.c` | `osAvailableCoreCount` (must respect affinity/cgroup limits) | `sched_getaffinity` |

Future OS-dependent domains (e.g. an `OsCrypto.c` for OS crypto facilities)
join as sibling files in every platform directory, with their contract added
to `vm/os/Os.h` (or a sibling `vm/os/OsCrypto.h` once a domain grows large).

Register the OS in the `ST_OS` block in `CMakeLists.txt` (same link-time model
as `ST_ARCH`; unsupported platforms fail at configure time). pthreads and BSD
sockets are used directly by the VM (POSIX-portable by design) — a Windows
port would need equivalents for those too, which is a bigger surface than
this directory.

## Future work (known, deliberate gaps)

- **Snapshot format**: endianness above; also `InstanceShape` is serialized by
  struct reinterpret, so struct layout/padding is part of the image format.
- **Primitive numbering**: `Primitives[]` in `vm/runtime/Primitives.c` is
  indexed by the primitive numbers stored in CompiledMethods (i.e. in the
  image). **Never reorder that table**; `PrimitivesGen.def` documents the
  generator contract but the table stays the source of truth for numbers.
- **Reader-side code-publish barriers** on weak-memory arches (the
  `memory-model` audit above).
