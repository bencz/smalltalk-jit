#ifndef TARGET_TRAITS_H
#define TARGET_TRAITS_H

// Compile-time facts about the selected CPU target that GENERIC code needs —
// deliberately tiny (constants only, no ISA model) so that consumers like
// vm/memory/HeapPage.c or vm/concurrency/Fiber.c do not drag the full arch
// assembler in. This is the second (and last) architecture #if in the tree,
// besides jit/TargetAssembler.h; keep both chains and the CMake ST_ARCH block
// in sync.
//
// Each vm/jit/<arch>/Traits<Arch>.h must define:
//   TARGET_CODE_FILLER_BYTE   byte pattern for fresh executable pages, chosen
//                             so a stray jump traps (x64: 0xCC INT3)
//   TARGET_STACK_GROWS_DOWN   1 on every currently supported target; the trait
//                             exists so an upward-stack port (HP-PA style)
//                             fails loudly at the sites that assume direction
//                             (grep PORT_ME(stack-direction)) instead of
//                             corrupting silently
//
// Byte order is NOT dispatched here: TARGET_BIG_ENDIAN comes portably from
// the compiler in vm/core/Endian.h.
#if defined(__x86_64__)
#include "jit/x64/TraitsX64.h"
#elif defined(__powerpc64__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#include "jit/ppc64/TraitsPpc64.h"
#elif defined(__powerpc64__)
#include "jit/ppc64le/TraitsPpc64le.h"
#else
#error "No target traits for this CPU. Create vm/jit/<arch>/Traits<Arch>.h and extend this dispatch - see PORTING.md."
#endif

#endif
