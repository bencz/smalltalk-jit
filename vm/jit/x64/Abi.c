// Shared delegates from generic x64 emitter names into the selected ABI
// instance (link-time bound via gX64Abi — see Abi.h). Kept as real functions
// (not header statics) so there is exactly ONE binding per process and the
// 12 asmLoadTls emit sites across the backend stay untouched by ABI swaps.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/Abi.h"
#include "jit/TargetCodePatch.h"
#include "jit/TargetEntry.h"
#include "core/CompiledCode.h"
#include "core/Endian.h"


void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	gX64Abi->emitLoadTls(buffer, dst, tpoff);
}


// Baked pointers live as the contiguous imm64 of a movabs on x64 — plain
// (alignment-safe) 8-byte accesses. See jit/TargetCodePatch.h.
uint64_t targetReadCodePointer(const uint8_t *site)
{
	return loadU64(site);
}


void targetWriteCodePointer(uint8_t *site, uint64_t value)
{
	storeU64(site, value);
}


// C -> JIT entry: on x64 the stub's code address IS a callable C function
// pointer. See jit/TargetEntry.h (ppc64 ELFv1 needs a descriptor instead).
Value targetCallSmalltalkEntry(void *entryStubInsts, void *arg0, void *arg1,
	Value *args, struct Thread *thread)
{
	union PointerConverter converter;
	converter.object_pointer = entryStubInsts;
	return converter.function_pointer((Value) arg0, (Value) arg1, args, thread);
}
