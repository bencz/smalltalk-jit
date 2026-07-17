#ifndef PPC64_PRIM_FRAME_H
#define PPC64_PRIM_FRAME_H

// Framed-primitive prologue/epilogue, shared between the generated
// primitives (PrimitivesPpc64.c) and the ELFv1 CCALL-primitive hook
// (abi/elfv1/AbiElfV1.c, which builds the fused sret frame itself).
//
// LR discipline rule 1: push LR first, so [FP+8] holds the send-site return
// address, byte-compatible with the x64 call-pushed one, then FP.
#include "jit/ppc64/AssemblerPpc64.h"

static inline void primFramedPrologue(AssemblerBuffer *buffer)
{
	asmMflr(buffer, R0);
	asmPush(buffer, R0);
	asmPush(buffer, FP);
	asmMr(buffer, FP, R1);
}

// Tear the frame down to FP, restore FP and LR. Emits NO blr: return paths
// add it; fail paths fall through into the method body (whose prologue
// re-pushes the restored LR).
static inline void primFramedEpilogue(AssemblerBuffer *buffer)
{
	asmMr(buffer, R1, FP);
	asmPop(buffer, FP);
	asmPop(buffer, R0);
	asmMtlr(buffer, R0);
}

#endif
