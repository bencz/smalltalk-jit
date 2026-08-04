#ifndef SSA_BACKEND_H
#define SSA_BACKEND_H

// Tier 2, end to end. See SsaBackend.c for the four places it can refuse and
// why refusing is always safe.

#include "jit/Jit.h"
#include "jit/Passes.h"
#include "jit/SsaEmitter.h"

// Compile `unit` with the SSA backend. `tier1` supplies the inline-cache cells,
// which the two tiers SHARE. Answers NULL and names the reason when any stage
// refuses, in which case tier 1's code stands.
//
// `stats` may be NULL; when it is not, it receives what the optimizer did.
// It exists so a caller can ask WHETHER SPECIALIZATION HAPPENED rather than
// infer it from the answer being right, which every unspecialized compilation
// also produces. A test that could not tell the two apart would keep passing
// after the profile stopped reaching the optimizer at all.
NativeCode *ssaCompile(const SsaEmitterOps *ops, CodeUnit *unit,
	NativeCode *tier1, const char **refused, PassStats *stats);

#endif
