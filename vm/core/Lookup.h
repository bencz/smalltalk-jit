#ifndef LOOKUP_H
#define LOOKUP_H

#include "core/Object.h"
#include "core/CompiledCode.h"
#include "runtime/String.h"
#include "core/Thread.h"
#include <stdint.h>

#define LOOKUP_CACHE_SIZE 4096

typedef struct {
	OBJECT_HEADER;
	Value selector;
	Value arguments;
} RawMessage;
OBJECT_HANDLE(Message);

typedef struct {
	RawClass *classes[LOOKUP_CACHE_SIZE];
	RawString *selectors[LOOKUP_CACHE_SIZE];
	uint8_t *codes[LOOKUP_CACHE_SIZE];
} LookupTable;

extern PER_ISOLATE LookupTable LookupCache;

// Offset of the per-thread LookupCache from the thread pointer (initial-exec TLS,
// identical on every thread; computed once in initThread like gCurrentThreadTpoff).
// The JIT send fast path reaches the cache with asmLoadTls(tpoff) so SHARED code
// probes the RUNNING worker's own cache. Baking `&LookupCache` as an immediate
// instead pins every worker to the CODEGEN thread's cache: that owner rewrites a
// 3-word entry in place (class, selector, code) while peers read it lock-free, and
// a torn read (old class+selector, new code) dispatches the WRONG nativeCode —
// under multi-worker socket load that meant receivers executed against another
// method's code (type confusion, frame corruption, hangs), with no GC involved.
extern ptrdiff_t gLookupCacheTpoff;

NativeCodeEntry lookupNativeCode(RawClass *class, RawString *selector);
NativeCode *getNativeCode(Class *class, CompiledMethod *method);


static intptr_t lookupHash(intptr_t classHash, intptr_t selectorHash)
{
	// class/selector are 16-byte-aligned heap addresses, so bits 0-3 of the XOR
	// are always zero. Shift them out before masking, otherwise only 1/16th of
	// the 4096 buckets are reachable (an effective 256-entry cache). The JIT
	// probe in generateMethodLookup() must apply the identical shift+mask.
	return ((classHash ^ selectorHash) >> 4) & (LOOKUP_CACHE_SIZE - 1);
}


static void flushLookupCache(void)
{
	memset(&LookupCache, 0, sizeof(LookupCache.classes) + sizeof(LookupCache.selectors));
}


static NativeCodeEntry cachedLookupNativeCode(RawClass *class, RawString *selector)
{
    intptr_t hash = lookupHash((intptr_t) class, (intptr_t) selector);
    if (LookupCache.classes[hash] == class && LookupCache.selectors[hash] == selector) {
        union PointerConverter converter;
        converter.object_pointer = LookupCache.codes[hash];
        return converter.function_pointer;
    }
    return lookupNativeCode(class, selector);
}

#endif
