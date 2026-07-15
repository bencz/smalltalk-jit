// Shared delegates from generic x64 emitter names into the selected ABI
// instance (link-time bound via gX64Abi — see Abi.h). Kept as real functions
// (not header statics) so there is exactly ONE binding per process and the
// 12 asmLoadTls emit sites across the backend stay untouched by ABI swaps.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/Abi.h"


void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	gX64Abi->emitLoadTls(buffer, dst, tpoff);
}
