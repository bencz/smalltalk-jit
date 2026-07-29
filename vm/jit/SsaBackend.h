#ifndef SSA_BACKEND_H
#define SSA_BACKEND_H

// Tier 2, end to end. See SsaBackend.c for the four places it can refuse and
// why refusing is always safe.

#include "jit/Jit.h"
#include "jit/SsaEmitter.h"

// Compile `unit` with the SSA backend. `tier1` supplies the inline-cache cells,
// which the two tiers SHARE. Answers NULL and names the reason when any stage
// refuses, in which case tier 1's code stands.
NativeCode *ssaCompile(const SsaEmitterOps *ops, CodeUnit *unit,
	NativeCode *tier1, const char **refused);

#endif
