#ifndef LOOKUP_H
#define LOOKUP_H

#include "Object.h"
#include "CompiledCode.h"
#include "String.h"
#include "Thread.h"
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
