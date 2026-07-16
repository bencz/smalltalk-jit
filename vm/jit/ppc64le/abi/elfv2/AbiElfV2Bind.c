// Binds the GENERIC ABI names to the ELFv2 instance. This is the only file
// that may define gPpc64leAbi, asmLoadTls and the jit/TargetFiber.h symbols:
// CMake's ST_ABI links exactly one Abi<Abi>Bind.c into a REAL ppc64le build.
// Foreign-host golden builds link the instance (AbiElfV2.c) WITHOUT this TU:
// there the generic names belong to the host backend's own binding.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vm/jit/ppc64le/ is LITTLE-ENDIAN ppc64le only (big-endian ppc64 has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/ppc64le/Abi.h"
#include "jit/ppc64le/abi/elfv2/FiberElfV2.h"
#include "jit/TargetFiber.h"
#include "jit/TargetEntry.h"
#include "core/CompiledCode.h"

const Ppc64leAbi *const gPpc64leAbi = &AbiPpc64leElfV2;

// C -> JIT entry (jit/TargetEntry.h). ELFv2 has NO function descriptors, so
// the stub's raw code address IS a callable C function pointer: this is the
// plain x64-shaped cast (vm/jit/x64/Abi.c), with none of the ELFv1 sibling's
// volatile-plus-asm-barrier dance, which existed only to stop GCC dead-storing
// the descriptor it had to synthesize on the stack.
//
// The compiler frames this as an ELFv2 indirect call: it puts the target in
// r12 and saves/restores its own r2 around the bctrl. JIT code is therefore
// free to clobber r2, since it never uses it and C callees establish their own
// TOC from r12 inside emitCallCFunction.
Value targetCallSmalltalkEntry(void *entryStubInsts, void *arg0, void *arg1,
	Value *args, struct Thread *thread)
{
	union PointerConverter converter;
	converter.object_pointer = entryStubInsts;
	return converter.function_pointer((Value) arg0, (Value) arg1, args, thread);
}

// TLS load delegate: same seam as x64 (emit-time only, no cost in generated
// code, since the vtable is dereferenced while EMITTING, not while running).
void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	gPpc64leAbi->emitLoadTls(buffer, dst, tpoff);
}

// The jit/TargetFiber.h contract names, statically bound to the ELFv2 pair:
// at -O2 these compile to direct tail-calls, so there is no vtable dereference
// on the context-switch path.
void fiberSwitchAsm(void **saveSp, void *newSp)
{
	fiberSwitchElfV2(saveSp, newSp);
}

void *fiberTargetPrimeStack(void *top, void (*entry)(void))
{
	return fiberPrimeStackElfV2(top, entry);
}
