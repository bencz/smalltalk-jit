#ifndef TARGET_CODE_PATCH_H
#define TARGET_CODE_PATCH_H

// Read/patch a BAKED 64-bit immediate (a young-object pointer registered in
// NativeCode's pointersOffsets, or an absolute code address planted by an
// ASM_FIXUP_SIZE_CODE_POINTER fixup) inside emitted machine code. On x64 the
// immediate is the contiguous imm64 of a movabs, so this is a plain 8-byte
// access; on ppc64 it is SPLIT across the four 16-bit halves of the fixed
// 5-instruction asmLi64 shape — no contiguous word exists, which is why the
// GC's pointer-patch loops (Scavenger.c / GarbageCollector.c) and the fixup
// binder (jit/Assembler.h) go through this link-time contract instead of
// dereferencing the site directly. Defined by each backend's
// CodeGenerator<Arch>.c. Callers own the icache flush after writing (the GC
// loops already do it).
#include <stdint.h>

uint64_t targetReadCodePointer(const uint8_t *site);
void targetWriteCodePointer(uint8_t *site, uint64_t value);

// Poison a speculation guard (see jit/SpecSite.h): rewrite the CONDITIONAL
// branch at `site` as an UNCONDITIONAL branch to the same target, so the
// guard's fallback edge is taken from now on. Both backends emit a fixed-width
// conditional branch for this purpose (x64 the 6-byte 0F 8x rel32, ppc64 the
// 4-byte bc), so the rewrite is always in-place and never needs to move code.
// The caller owns the world-stopped bracket and the icache flush.
void targetPoisonGuardBranch(uint8_t *site);

// Poison a compile-time devirtualized send (SPEC_STATIC in jit/SpecSite.h):
// `site` is the UNCONDITIONAL branch that skips the send's inline re-resolve
// thunk, and this zeroes its displacement so control falls into the thunk
// instead. Idempotent, like targetPoisonGuardBranch. Same world-stopped and
// icache-flush contract.
void targetPoisonStaticSkip(uint8_t *site);

#endif
