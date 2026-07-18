// Arch-NEUTRAL half of the stub machinery: the per-heap double-checked stub
// cache and the CodeGenerator initializer. The stubs' actual emission
// (StubCode.generator) and generateStubCall live in the selected backend.
// Hoisted out of the x64 backend so a new arch's skeleton only provides
// emitters, not cache plumbing.
#include "jit/StubCode.h"
#include "jit/CodeGenerator.h"
#include "core/Thread.h"
#include "core/Handle.h"
#include "core/CompiledCode.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"


void initCodeGenerator(CodeGenerator *generator)
{
	asmInitBuffer(&generator->buffer, 256);
	generator->code.methodOrBlock = NULL;
	// The RegsAlloc arrays are heap-allocated lazily (regsAllocEnsure /
	// computeRegsAlloc) and released by freeCodeGenerator's regsAllocFree;
	// stubs never allocate them.
	memset(&generator->regsAlloc, 0, sizeof(generator->regsAlloc));
	generator->frameSize = 0;
	generator->frameRawAreaSize = 0;
	generator->tmpVar = 0;
	generator->bytecodeNumber = 0;
	generator->overapproxStackmap = 0;
	generator->tierFeedback = NULL;
	generator->tierSiteMap = NULL;
	generator->tierSiteMapSize = 0;
	generator->stackmaps = newOrdColl(8);
	// NULL = "no bytecode descriptors" (stubs); method/block compilation
	// allocates its own collection right after init.
	generator->descriptors = NULL;
}


NativeCode *getStubNativeCode(StubCode *stub)
{
	Heap *heap = CurrentThread.heap;
	NativeCode *code = __atomic_load_n(&heap->stubCode[stub->id], __ATOMIC_ACQUIRE);
	if (code != NULL) {
		return code;
	}
	heapCodegenLockEnter(heap); // serialize codegen across this heap's workers
	code = __atomic_load_n(&heap->stubCode[stub->id], __ATOMIC_ACQUIRE); // re-check under lock
	if (code == NULL) {
		HandleScope scope;
		openHandleScope(&scope);

		CodeGenerator generator;
		initCodeGenerator(&generator);
		stub->generator(&generator);
		code = buildNativeCode(&generator);
		if (generator.code.methodOrBlock != NULL) {
			compiledMethodSetNativeCode((CompiledMethod *) generator.code.methodOrBlock, code);
		}
		asmFreeBuffer(&generator.buffer);
		__atomic_store_n(&heap->stubCode[stub->id], code, __ATOMIC_RELEASE); // publish last

		closeHandleScope(&scope, NULL);
	}
	heapCodegenLockLeave(heap);
	return code;
}
