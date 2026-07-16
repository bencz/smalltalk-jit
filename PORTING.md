# Porting the VM to a new CPU or OS

The tree is organized so that CPU- and OS-specific code lives in exactly two
places, bound at **link time** (CMake decides which files to compile; generic
code calls through neutral contract headers). There is deliberately no
`#ifdef` forest: the only architecture `#if` in the tree is the dispatch chain
in `vm/jit/TargetAssembler.h`, plus an `#error` guard at the top of each
backend source.

```
vm/jit/<arch>/            ALL CPU-specific code (assembler, codegen, stubs,
                          primitive generators)
vm/jit/<arch>/abi/<abi>/  the platform C-ABI binding for that backend
                          (calling convention, TLS access, fiber switch)
                          as an ops-struct — see "The C-ABI axis" below
vm/os/<os>/               ALL OS-specific code behind the vm/os/Os.h contract,
                          split by domain (OsTime, OsMemory, OsEvents,
                          OsSignals, OsCpu, ... one file per domain)
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
| endianness | `vm/tools/Snapshot.c` | RESOLVED by image format v2: a self-describing header (magic/version/byte-order/word-size) plus struct-layout-independent shape/object-header encodings. Images stay NATIVE-endian and per-build (a BE build reads/writes its own images correctly); a foreign-endian or legacy image is refused with an actionable error. Multi-byte bytecode fields are memcpy-based (alignment-safe). |
| `PORT_ME(memory-model)` | `vm/memory/Safepoint.h` | All cross-thread protocols use `__atomic` acquire/release builtins — portable in principle, but validated only under x86-TSO. Audit every publish/observe pair (safepoint flags, JIT code publish, remembered sets) before the first weak-memory port. |
| `PORT_ME(frame-layout)` | `vm/core/StackFrame.c` | The frame walker's slot arithmetic mirrors the frame the JIT emits. A backend must reproduce that layout (saved BP at `*frame`, slots growing down) or take over these accessors. |
| `PORT_ME(addr-tagging)` | `vm/core/Object.h` | Bit 3 of an object's *address* distinguishes young/old space. Keep 16-byte object alignment and page mappings that leave that bit meaningful. Arch-neutral, but an invariant to respect. |
| 64-bit only | `vm/core/Object.h` | `_Static_assert(sizeof(Value) == 8)`: tagged 62-bit ints and the descriptor bit-packing assume 64-bit `Value`. 32-bit targets are out of scope. |

## The C-ABI axis (`vm/jit/<arch>/abi/<abi>/`)

Within one instruction set, the **platform C ABI** still varies: x86-64 SysV
(Linux/macOS) vs Win64 differ in argument registers, shadow space,
callee-saved sets, struct return, TLS access and the fiber-switch register
set; ppc64 carries the same split as ELFv2 (little-endian) vs ELFv1 (AIX /
big-endian). This is the third link-time axis, subordinate to the arch:

| ST_ARCH | ST_OS | ST_ABI |
|---|---|---|
| x64 | linux, osx | sysv |
| x64 | windows | win64 (reserved) |
| ppc64le | linux | elfv2 |
| ppc64 | aix / linux BE | elfv1 |

The binding is an **ops-struct (vtable)** — `X64Abi` in `vm/jit/x64/Abi.h`:
data tables (arg registers, callee-saved map, caller-saved spill order, shadow
space, entry-stub save size) plus emitter hooks (TLS load, CCALL-primitive arg
marshalling, `PrimitiveResult` decode, entry-stub save/restore) and the fiber
pair. Every member is consumed at JIT-**emit** time, so generated code pays
zero indirection; the fiber switch is bound by direct-call wrappers in the
selected `Abi<Abi>Bind.c` (no vtable dereference on the context-switch path).

What is deliberately NOT in the vtable: the **VM-internal calling convention**
(args/receiver on the Smalltalk stack, native-code target in R11, result in
RAX, CTX=R12, TMP=R10, push-rbp frames, the register-allocation pool). It is
identical under SysV and Win64 — the registers the JIT keeps live across C
calls ({RBX, R12-R15}) are callee-saved in both — and must not diverge per ABI.

**Instance/Bind split**: `abi/sysv/AbiSysV.c` + `FiberSysV.c` export only
sysv-suffixed symbols and are compilable on any x64 host; all generic names
(`gX64Abi`, `fiberSwitchAsm`, `fiberTargetPrimeStack`) live exclusively in
`AbiSysVBind.c`, which CMake's `ST_ABI` selection links exactly once. That is
what lets a test binary link SEVERAL ABI instances side by side and
golden-test a foreign ABI's emitters on this host — see the next section.

**Golden emission tests** (`vm/tests/EmitGoldenX64.c`, `ST_ABI_EMIT_TEST=1`):
five representative sequences (TLS load, `generateCCall`, the store-check
spill run, the CCALL-primitive trampoline, the whole entry stub) emitted
through the real generators and compared byte-for-byte against pinned vectors
(`EmitGoldenX64Expected.h`); non-fake pointer immediates are masked, so the
vectors are build-independent. `ST_ABI_EMIT_TEST=print` regenerates them.
A cross-member invariant check also runs: the spill list must equal exactly
the C-clobbered subset of the JIT's live set, and argument registers must be
volatile — that is what keeps a NEW ABI instance honest before any code runs.

### Win64 checklist (the six silent breaks, mapped to their seam member)

1. **`PrimitiveResult` hidden-pointer return** — the 16-byte struct returns
   via sret in RCX on Win64 (SysV: RAX:RDX), shifting every argument right by
   one. Implement in `emitCCallPrimArgs` (reserve the sret slot, shift args)
   + `emitPrimResultCheck` (read the slot, release it on both paths). Affects
   all ~71 CCALL primitives through the one shared trampoline. Max CCALL
   arity is 5 → 6 Win64 slots → 2 stack args, which also forces item 2.
2. **Shadow space + stack args** — `shadowSpace = 32` (the consumption point
   in `generateCCall` already exists and emits nothing on SysV). Calls with
   more than 4 slots need call-time stack args staged after the alignment
   `and`; upgrade the shadow-space point into an `emitCCallFrame(buffer,
   argsSize)` hook (the currently-unread `argsSize` parameter of
   `generateCCall` is the pre-drilled hole). Also audit the fixed
   `generateCCall` sites that hand-place C args in SysV registers
   (`grep -n 'generateCCall(' vm/jit/x64/*.c` and read a few lines above
   each) — each needs per-ABI placement or a shuffle hook.
3. **Fiber switch** — Win64's nonvolatile set adds RDI, RSI and XMM6-XMM15
   (a `movdqa` save area, not pushes), and the TEB stack fields
   (`StackBase`/`StackLimit`/`DeallocationStack`) must be swapped per switch
   or SEH/stack probing misbehaves. A new `abi/win64/FiberWin64.c` +
   `fiberPrimeStackWin64` with the matching slot layout.
4. **Entry stub / spill deltas** — `entrySavedRegsSize` grows (+RDI/RSI/XMM),
   the save/restore hooks change accordingly, and `callerSavedSpill`
   *shrinks* (RSI/RDI become callee-saved). The golden invariant check
   recomputes the required spill set automatically.
5. **TLS** — `%fs`-based initial-exec does not exist on Windows: `emitLoadTls`
   must go through the `%gs` TEB (`ThreadLocalStoragePointer[TlsIndex]`), and
   the `__builtin_thread_pointer()`-based tpoff derivation in
   `vm/core/Thread.c` is invalid there — a per-OS offset-derivation story is
   needed alongside the emitter.
6. **Unwind/misc** — Win64 wants `.pdata`/`RtlAddFunctionTable` for JIT
   frames (SEH can unwind through them), `__chkstk` probes for >4 KB frames,
   and `AL` zeroed before variadic C calls (none exist today — assert if one
   appears).

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

## ppc64 (big-endian ELFv1 first, then little-endian ELFv2)

Status: **the ppc64 (BE) port is COMPLETE through the full JIT** —
`CodeGeneratorPpc64.c`, `StubCodePpc64.c` and `PrimitivesPpc64.c` are real
(a 1:1 translation of the x64 backend under the mapping in
`vm/jit/ppc64/DESIGN.md`, which also documents the seven bring-up bugs that
were root-caused on the way — read it before porting the next arch).
Verified: bootstrap produces a byte-count-identical image, `tests/` 40/40 and
the suite's samples all pass under qemu-user, and `./build.sh` (cmake + full
native compile + the whole test suite) runs green inside a native ppc64 BE
debian-ports rootfs (`mmdebstrap --mode=chrootless` + `podman import`; the
host's binfmt qemu makes the container transparently executable).
**The ppc64le (LE/ELFv2) port is COMPLETE too**: `./build.sh` runs green
(`ALL PASSED`) inside the OFFICIAL `docker.io/library/debian --platform
linux/ppc64le` container, which needs no hand-built rootfs at all, and under
qemu-user the bootstrap produces a byte-count-identical image with `tests/`
40/40 and all samples passing. Its deltas from this backend, and the ELFv2
facts DERIVED FROM THE TOOLCHAIN rather than from the spec (with the BE cross
as the control that validates the method), are in `vm/jit/ppc64le/DESIGN.md`.
Read it before assuming anything about ELFv2.

They are SEPARATE backends by design: the ecosystem treats them as distinct
architectures and the codegen diverges beyond the ABI. `vm/jit/ppc64le/` is a
deliberate COPY, not a refactor, and the two assemblers now `#error` if a
translation unit tries to include both (each fixes the instruction word's byte
order file-wide). Only SIX of the assembler's 101 functions actually differ;
the other 95 pack bit-fields into a `uint32_t` and are byte-order-neutral. The
endianness/portability hazards found by the deep audit are FIXED in generic
code: snapshot format v2 is self-describing (magic/version/byte-order header,
struct-layout-independent shape encoding, loud refusal of foreign images),
bytecode multi-byte fields are memcpy-based (alignment-safe), the tokenizer
indexes its tables with `unsigned char` (ppc64/s390x/arm have unsigned plain
char), `tagChar` normalizes through unsigned char, the GC's madvise uses
`osPageSize()` (ppc64 distros default to 64 KB pages), and CityHash gets
`WORDS_BIGENDIAN` on BE builds.

What exists (rung 1):

- **Encoders** (`vm/jit/ppc64/AssemblerPpc64.h`): ld/std/stdu, lwz/stw,
  lfd/stfd, addi/addis/li/lis, ori/oris/or/mr/nop, rldicr/sldi, cmpdi,
  mfspr/mtspr (LR/CTR) + mfcr/mtcrf, blr/bctr/bctrl, b/bc with
  ppc64-specific label emit/bind (branch displacements are relative to the
  instruction's OWN address and live inside the word — asmPpcLabelBind
  patches the big-endian word, telling I-form from B-form by opcode), and
  `asmLi64` — the movabs analog: a FIXED 5-instruction shape
  (lis/ori/sldi 32/oris/ori) for any 64-bit immediate, patchable in place by
  `asmLi64Patch` (what a GC pointer-patch loop must use).
- **HOST-INDEPENDENT emission**: encoders store words EXPLICITLY big-endian
  (== native order on every host the backend executes on), so the x86 dev
  host compiles them, links the ELFv1 instance into its own `st`, and runs
  the full golden natively: `ST_PPC64_EMIT_TEST=1|print` (19th self-test).
  Pinned vectors: `vm/tests/EmitGoldenPpc64Expected.h`, validated
  byte-identical against the debian cross binutils by
  `scripts/ppc64/golden-oracle.sh` (hand-maintained reference `.s` — an
  independent derivation, which is what makes it an oracle). The same test
  re-runs on a real BE target in rung 2 (`cross-test.sh be`).
- **`Ppc64Abi` ops-struct** (`vm/jit/ppc64/Abi.h`, mirroring `X64Abi`):
  argRegs r3-r10, callee-saved map (r1/r2/r13 reserved), caller-saved spill
  table (PLACEHOLDER contract: pool volatiles + r3-r6, invariant-checked by
  the golden), `paramSaveArea = 64` (the ELFv1 shadow-space analog),
  `emitLoadTls` (addis/addi ha/lo from r13 — the 0x7000 bias cancels out
  because tpoffs are computed at runtime from r13 itself),
  `emitCallCFunction` (the `.opd` descriptor dance: save r2 to 40(r1), load
  entry/TOC/env from the descriptor, mtctr/bctrl, restore r2), entry-stub
  save/restore hooks (the full nonvolatile stdu frame), and the fiber pair.
  `emitCCallPrimArgs`/`emitPrimResultCheck` FAIL() — see the sret hazard
  below.
- **Instance/Bind/asm split** (`vm/jit/ppc64/abi/elfv1/`): `AbiElfV1.c`
  (instance + all emitter hooks + the C fiber PRIME) is host-independent;
  `FiberElfV1.c` (the fiber SWITCH, real POWER asm with its own `.opd`
  descriptor) is the ONLY arch-only TU and foreign hosts link a FAIL() stub
  (`vm/tests/EmitGoldenPpc64HostStub.c`) in its place; `AbiElfV1Bind.c`
  binds the generic names (gPpc64Abi, asmLoadTls, TargetFiber.h) on real
  ppc64 builds only. Fiber frame: 336-byte ELFV1_NV frame (back chain via
  stdu, CR/LR in the CALLER header slots, TOC at 40, r14-r31 at 48, f14-f31
  at 192 — ppc64 HAS nonvolatile FP regs, unlike SysV x64). The prime half
  dereferences the entry DESCRIPTOR at prime time (entry address → LR slot,
  TOC → r2 slot); its layout is checked natively by the golden.

### Bring-up ladder (in order)

0. **Derive the ABI facts from the TOOLCHAIN, not from the spec** (the step
   that made the LE port land clean). Compile a probe `-O2 -S` with the target
   cross gcc and read what it actually emits for the shapes you must
   reproduce: how a 16-byte aggregate comes back, what frame an 8-argument
   indirect call builds, where the TOC is saved, what the global entry does.
   Then run the SAME probe through a cross whose ABI you already know (here:
   ELFv1) and check it reproduces the facts you already trust. That control is
   what makes the new answers evidence rather than recollection. Everything the
   LE port assumed up front about "endianness deltas" in the codegen turned out
   to be wrong; everything it derived from gcc was right.
1. **Golden emission, natively on x86: DONE for BE and LE** (see above; each
   port has its own explicitly-byte-ordered emitters, vectors and env var, and
   `scripts/ppc64/golden-oracle.sh <be|le>` validates both against binutils).
2. **Non-JIT tests under qemu-user** — `./scripts/ppc64/cross-test.sh be|le`
   (VALIDATED on both byte orders): cross-compiles STATIC test binaries at
   native speed inside an x86_64 debian container (debian's
   `gcc-powerpc64{,le}-linux-gnu` + `libc6-dev-ppc64{,el}-cross` ship a full
   libc, unlike Fedora's kernel-only cross packages), then runs them directly
   on the host through binfmt/`qemu-user-static`. Covers TokenizerTest,
   ST_SAFEPOINT_TEST, ST_TLAB_TEST, ST_SNAPSHOT_FORMAT_TEST (the big-endian
   fire test for the image format) and ST_ABI_EMIT_TEST, each backend's own
   pinned golden vectors, re-checked on a genuinely big/little-endian target.
   Host prerequisite (once): `sudo dnf install -y qemu-user-static-ppc`. Gotchas
   learned: pin `--platform` on every podman run (a cached foreign-arch image
   can hijack a tag) and mount shared volumes with `:z`, not `:Z` (concurrent
   containers relabel each other's access away).
3. **Full build + suite on a native rootfs: DONE for both.** BE needs a
   hand-built Debian ports rootfs (`mmdebstrap --mode=chrootless` +
   `podman import`); LE just uses the OFFICIAL image
   (`--platform linux/ppc64le docker.io/library/debian`), which is why LE is by
   far the cheaper POWER target to keep green. The host's binfmt qemu makes
   either container transparently executable, and `./build.sh` runs cmake, a
   native compile, the bootstrap and the whole suite inside it.
4. **Faithful signals/SMP under qemu-system**: still PENDING for both. qemu-user
   is a syscall emulator: it delivers `si_addr = NULL` on guard faults (hence
   `ST_FIBER_PRECOMMIT=1`) and does not model SMP memory ordering, so the
   concurrency battery on real POWER hardware is not yet trustworthy. That, plus
   the `PORT_ME(memory-model)` audit, is what stands between these ports and a
   multi-worker deployment.

### ELFv1 (BE) facts the backend must implement

- **Function descriptors**: a C function pointer points at an `.opd` entry
  `{entry, TOC, environ}` — implemented in `emitCallCFunction` (and the
  fiber prime, which reads the descriptor in C). The future ppc64
  `generateCCall` builds its frame around that hook. (ELFv2 instead has
  global/local entry points — `localentry` — and r12 must hold the entry
  address at global entry.)
- **PrimitiveResult sret** — PORT_ME(elfv1-sret): ELFv1 returns ALL structs
  (including the 16-byte PrimitiveResult) through a HIDDEN pointer in r3,
  shifting every argument right by one — the same class of silent break as
  Win64 item 1. This is why ppc64's `generateCCallPrimitive` is a FUSED
  CCall+marshal+decode sequence (the sret buffer lives inside the ABI frame and
  must be read back BEFORE teardown) while ppc64le uses the plain x64
  structure, the single biggest structural divergence between the two POWER
  backends, and the reason they are separate files.
- **TOC (r2)**: reserved; save/restore across cross-module calls per ABI
  (`emitCallCFunction` uses the 40(r1) slot).
- **No push/pop**: frames are `stdu r1, -N(r1)` with a back-chain word; the
  fiber switch and entry-stub hooks save the nonvolatiles (r14-r31, f14-f31,
  CR, LR) into the shared ELFV1_NV frame shape. **v20-v31/VSX are NOT
  saved** — audit before enabling VMX/VSX anywhere (compiler flags on the C
  side count too, if callers ever hold vector values across a switch).
- **TLS**: r13 is the thread pointer; the ABI's **0x7000 bias** cancels out
  of our runtime-computed tpoffs (Thread.c derives them FROM r13 via
  `targetThreadPointer()`), so `emitLoadTls` is a plain ha/lo add — no bias
  constant appears anywhere. The bias only matters if link-time TPREL
  relocations ever get involved (they don't today).
- **I-cache**: not coherent with stores — after writing code:
  `dcbst; sync; icbi; isync` per block (`__builtin___clear_cache` handles the
  local hart via `osFlushICache`, already called at every publish point);
  CROSS-CORE publish needs the memory-model audit (`PORT_ME(memory-model)`).
- **Pages**: 64 KB default on most ppc64 distros — everything goes through
  `osPageSize()` already; keep it that way.
- **Weak memory model**: run the acquire/release audit before trusting the
  multi-worker scheduler (x86-TSO hid any missing barrier so far).

## Future work (known, deliberate gaps)

- **Snapshot format**: v2 resolved the endianness/struct-padding hazards; the
  remaining stance is that images are per-endianness artifacts by design
  (cross-endian image EXCHANGE is a non-goal — heap Values punned from C
  structs, e.g. the bootstrap `*Shape` globals, are only self-consistent).
- **Primitive numbering**: `Primitives[]` in `vm/runtime/Primitives.c` is
  indexed by the primitive numbers stored in CompiledMethods (i.e. in the
  image). **Never reorder that table**; `PrimitivesGen.def` documents the
  generator contract but the table stays the source of truth for numbers.
- **Reader-side code-publish barriers** on weak-memory arches (the
  `memory-model` audit above).
