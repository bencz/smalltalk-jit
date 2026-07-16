#include "core/Lookup.h"
#include "jit/CodeGenerator.h"
#include "core/Object.h"
#include "core/Class.h"
#include "runtime/String.h"
#include "core/Smalltalk.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/CompiledCode.h"
#include "jit/CodeDescriptors.h"
#include "core/Thread.h"
#include "core/StackFrame.h"

PER_ISOLATE LookupTable LookupCache = {
	.classes = { NULL },
	.selectors = { NULL },
	.codes = { NULL },
};

static NativeCodeEntry doesNotUnderstand(Class *class, String *selector);

NativeCodeEntry lookupNativeCode(RawClass *class, RawString *selector)
{
    HandleScope scope;
    openHandleScope(&scope);

    Class *classHandle = scopeHandle(class);
    String *selectorHandle = scopeHandle(selector);
    CompiledMethod *method = lookupSelector(classHandle, selectorHandle);

    NativeCodeEntry entry;
    if (method == NULL) {
        entry = doesNotUnderstand(classHandle, selectorHandle);
    } else {
        union PointerConverter converter;
        converter.object_pointer = getNativeCode(classHandle, method)->insts;
        entry = converter.function_pointer;
    }

    intptr_t hash = lookupHash((intptr_t) classHandle->raw, (intptr_t) selectorHandle->raw);
    LookupCache.classes[hash] = classHandle->raw;
    LookupCache.selectors[hash] = selectorHandle->raw;

    union PointerConverter converter;
    converter.function_pointer = entry;
    LookupCache.codes[hash] = converter.object_pointer;

    closeHandleScope(&scope, NULL);
    return entry;
}

// Type feedback was collected here on every lookup-cache miss but never consumed
// (the Optimizer's feedback-driven inliner is not wired up), so it was pure
// allocation + GC-tracing overhead and has been removed. `NativeCode.typeFeedback`
// now stays NULL and the GC walkers skip it. Revive deliberately if/when an
// adaptive recompiler is added.


static NativeCodeEntry doesNotUnderstand(Class *class, String *selector)
{
    intptr_t hash = lookupHash((intptr_t) class->raw, (intptr_t) selector->raw);
    NativeCode *code = generateDoesNotUnderstand(selector);
    code->compiledCode = lookupSelector(class, Handles.doesNotUnderstandSymbol)->raw;

    union PointerConverter converter;
    converter.object_pointer = code->insts;
    return converter.function_pointer;
}


NativeCode *getNativeCode(Class *class, CompiledMethod *method)
{
	(void) class;
	// Lock-free fast path: already-compiled methods (the common case) just ACQUIRE-load.
	NativeCode *code = compiledMethodGetNativeCode(method);
	if (code != NULL) {
		return code;
	}
	// Cold method: compile under codegenLock with double-checked locking, so concurrent
	// workers first-calling the same method don't double-compile or race the publish
	// (was: unlocked check + generate + non-atomic store → torn code pointer on a peer =
	// jump into a half-published NativeCode). Mirrors getStubNativeCode (StubCodeX64.c).
	// heapCodegenLockEnter is re-entrant (generateMethodCode takes it again) and counts
	// waiting as GC-safe.
	Heap *heap = CurrentThread.heap;
	heapCodegenLockEnter(heap);
	code = compiledMethodGetNativeCode(method); // re-check under the lock
	if (code == NULL) {
		code = generateMethodCode(method);
		compiledMethodSetNativeCode(method, code); // atomic RELEASE publish, last
	}
	heapCodegenLockLeave(heap);
	return code;
}


// Called by a mutator on ITSELF whenever it resumes past a point where a
// collection may have run (safepoint poll, leave-blocked, own collection).
// A scavenge moves young classes/selectors; the TLS LookupCache would keep
// dangling from-space pointers that, once that memory is reused, can FALSE-HIT
// and dispatch the wrong native code. Epoch-gated so the memset only runs
// once per actual collection.
void lookupCacheOnGcResume(Thread *self)
{
	if (self->heap != NULL && self->lookupCacheEpoch != self->heap->gcEpoch) {
		self->lookupCacheEpoch = self->heap->gcEpoch;
		flushLookupCache();
	}
}
