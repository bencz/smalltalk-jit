#ifndef SSA_RUNTIME_H
#define SSA_RUNTIME_H

// The runtime entry points only the SSA backend calls. See SsaRuntime.c for why
// they are calls and why they do not live in jit/Jit.c.

#include "core/Object.h"

Value jitBoxFloat(void *unused, Value *slot, uint64_t packed);
Value jitUnboxFloat(void *unused, Value *slot, uint64_t packed);
Value jitBoxInteger(void *unused, Value *slot, uint64_t packed);
Value jitUnboxInteger(void *unused, Value *slot, uint64_t packed);

#endif
