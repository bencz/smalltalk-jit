#ifndef OS_H
#define OS_H

#include <stdint.h>
#include <stddef.h>

// The operating-system seam. Bound at link time: CMake's ST_OS selection
// compiles exactly one platform directory — vm/os/<os>/ (linux today; windows/
// osx/aix are future peers), whose files implement this contract split by
// domain (OsTime, OsMemory, OsEvents, OsSignals, OsCpu — future domains like
// OsCrypto slot in as sibling files). The API is 1:1 with what the VM needs —
// no speculative generality — but shaped so a runtime-selectable backend (an
// ops struct, e.g. io_uring vs epoll) could slide in later without touching
// the callers.

// ---- time -------------------------------------------------------------------

int64_t osCurrentMicroTime(void);

// ---- virtual memory ---------------------------------------------------------

// System page size in bytes (cached after the first call).
long osPageSize(void);

// Committed RW (optionally executable) anonymous memory; NULL on failure.
// Executable mappings are RWX today — see PORT_ME(wxorx) at the call site.
void *osPageAlloc(size_t size, _Bool executable);

// Reserve ADDRESS SPACE only (no RAM, no overcommit charge, faults on touch);
// NULL on failure. Used for growable fiber stacks.
void *osPageReserve(size_t size);

// Commit (make RW) a span inside an osPageReserve'd region. Async-signal-safe
// (called from the SIGSEGV growth handler). Returns 1 on success.
_Bool osPageCommit(void *addr, size_t size);

// Return committed pages to the OS but keep the mapping: the span stays valid
// and re-faults as zero-filled on next touch.
void osPageRelease(void *addr, size_t size);

// Unmap a region created by osPageAlloc/osPageReserve. Returns 1 on success.
_Bool osPageFree(void *addr, size_t size);

// Make freshly written instructions visible to this core's instruction fetch.
// No-op on x86 (coherent I-cache). PORT_ME(icache): required on ARM/RISC-V/PPC;
// CROSS-thread code publication additionally needs a per-arch audit (RISC-V
// fence.i is hart-local) — see PORTING.md.
void osFlushICache(void *start, size_t size);

// ---- event loop (I/O readiness + poller wakeup) ------------------------------

// Opaque readiness multiplexer (epoll + wake-eventfd on Linux). One per
// scheduler; a single worker at a time blocks in osEventLoopWait.
typedef struct OsEventLoop OsEventLoop;

// Tag value reserved for the internal wakeup channel; never use it for a waiter.
#define OS_EVENT_TAG_RESERVED (~(uint64_t) 0)

OsEventLoop *osEventLoopCreate(void);

// Arm ONE-SHOT readiness (read or write) for fd, tagging the event with `tag`
// (a fiber id). Re-arms an fd the loop already knows.
void osEventLoopArm(OsEventLoop *loop, int fd, _Bool forWrite, uint64_t tag);

// Forget an armed fd (e.g. its waiting fiber was killed).
void osEventLoopDisarm(OsEventLoop *loop, int fd);

// Block until readiness or timeoutMs (-1 = forever; 0 = poll). Fills tags[]
// with the tags of ready events and returns how many. Wakeups via
// osEventLoopWake are consumed internally and are NOT returned as tags (they
// just cause an early return, possibly with 0 tags).
int osEventLoopWait(OsEventLoop *loop, uint64_t *tags, int maxTags, int timeoutMs);

// Kick the blocked waiter (if any) out of osEventLoopWait. Callable from any
// thread.
void osEventLoopWake(OsEventLoop *loop);

// ---- signals ------------------------------------------------------------------

// Install the process-global SIGSEGV handler (once) and this thread's alternate
// signal stack (per call). On a fault the handler calls `cb(faultAddress)` —
// async-signal-safe code only! — and retries the faulting instruction when cb
// returns 1 (the fault was consumed, e.g. a fiber stack grew). Otherwise the
// default handler is restored and the fault re-raised for a core dump at the
// real PC. No-op under a sanitizer build (the sanitizer owns SIGSEGV).
void osInstallSegvHandler(_Bool (*cb)(uintptr_t faultAddr));

// Writing to a peer-closed fd must return EPIPE, not kill the process.
void osIgnoreBrokenPipe(void);

// ---- scheduling ----------------------------------------------------------------

// CPUs this process may actually run on — respects taskset/cgroup limits
// (unlike _SC_NPROCESSORS_ONLN). At least 1.
int osAvailableCoreCount(void);

// ---- CPU feature discovery -------------------------------------------------

// Fill in this platform's raw CPU-feature words for the RUNNING architecture,
// in the vocabulary that vm/jit/<arch>/Cpu.h defines. Returns 0 (leaving the
// words untouched) when the platform offers no such mechanism, in which case
// the JIT stays at its ISA baseline.
//
// This lives on the OS axis, not the arch axis, because the ACQUISITION is an
// OS fact even when the features are architectural:
//   Linux    the ELF auxiliary vector, getauxval(AT_HWCAP/AT_HWCAP2). Note
//            AT_HWCAP is Linux/ELF ABI, NOT a POWER or ARM facility.
//   AIX      no auxv at all: _system_configuration.implementation
//            (<sys/systemcfg.h>, PV_970/PV_5/PV_6/PV_7...), which a port must
//            MAP into the arch's vocabulary. PASE follows AIX.
//   FreeBSD  elf_aux_info(AT_HWCAP, ...)
//   macOS    sysctlbyname("hw.optional.*")
// PORT_ME(cpu-features): implement per OS; returning 0 is always safe.
//
// x64 is the exception that proves the split: CPUID is an unprivileged
// INSTRUCTION, so vm/jit/x64/CpuX64.c reads it directly on every OS and never
// calls this. On POWER the equivalent register (the PVR, SPR 287) is
// supervisor-only, so the kernel must hand the answer over instead.
_Bool osCpuFeatureWords(uint64_t *word0, uint64_t *word1);

#endif
