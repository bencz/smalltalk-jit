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

#endif
