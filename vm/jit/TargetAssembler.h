#ifndef TARGET_ASSEMBLER_H
#define TARGET_ASSEMBLER_H

// Compile-time dispatch to the CPU backend's assembler: the Register enum, the
// TMP/CTX register assignments, the calling-convention tables and the
// instruction emitters. This is the ONLY architecture #if in the tree — every
// other arch binding is done at link time (CMakeLists.txt selects which
// vm/jit/<arch>/*.c files to compile via ST_ARCH). Keep this chain and the
// CMake ST_ARCH block in sync; both fail loudly when they disagree.
//
// Porting a new CPU: create vm/jit/<arch>/ (see PORTING.md for the full
// contract), add its assembler header here, and register the arch in
// CMakeLists.txt.
#if defined(__x86_64__)
#include "jit/x64/AssemblerX64.h"
#else
#error "No JIT backend for this CPU. Create vm/jit/<arch>/ and extend this dispatch - see PORTING.md."
#endif

#endif
