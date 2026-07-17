// Binds the GENERIC ABI names to the ELFv1 instance. This is the only file
// that may define gPpc64Abi, asmLoadTls and the jit/TargetFiber.h symbols ,
// CMake's ST_ABI links exactly one Abi<Abi>Bind.c into a REAL ppc64 build.
// Foreign-host golden builds link the instance (AbiElfV1.c) WITHOUT this TU:
// there the generic names belong to the host backend's own binding.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "vm/jit/ppc64/ is BIG-ENDIAN ppc64 only (ppc64le has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/ppc64/Abi.h"
#include "jit/ppc64/abi/elfv1/FiberElfV1.h"
#include "jit/TargetFiber.h"
#include "jit/TargetEntry.h"
#include <string.h>

const Ppc64Abi *const gPpc64Abi = &AbiPpc64ElfV1;

// C -> JIT entry (jit/TargetEntry.h). Under ELFv1 a C function pointer is a
// FUNCTION DESCRIPTOR pointer {entry, TOC, environ}: synthesize one on the
// stack around the stub's raw code address. The current TOC value is
// irrelevant to JIT code (it never uses r2; C callees reload theirs from
// their own descriptors via emitCallCFunction) but keeps the slot honest.
Value targetCallSmalltalkEntry(void *entryStubInsts, void *arg0, void *arg1,
	Value *args, struct Thread *thread)
{
	register void *toc __asm__("r2");
	__asm__("" : "=r"(toc));
	// volatile + the escape barrier below: the indirect call reads the
	// descriptor words through the ABI, invisibly to the compiler's alias
	// analysis, without them GCC dead-stores the array and CTR loads junk.
	volatile uintptr_t descriptor[3];
	descriptor[0] = (uintptr_t) entryStubInsts;
	descriptor[1] = (uintptr_t) toc;
	descriptor[2] = 0;
	__asm__ volatile("" : : "r"(descriptor) : "memory");

	typedef Value (*EntryFn)(void *, void *, Value *, struct Thread *);
	EntryFn fn;
	void *descriptorPointer = (void *) descriptor;
	memcpy(&fn, &descriptorPointer, sizeof(fn));
	return fn(arg0, arg1, args, thread);
}

// TLS load delegate: same seam as x64 (emit-time only, no cost in generated
// code, the vtable is dereferenced while EMITTING, not while running).
void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	gPpc64Abi->emitLoadTls(buffer, dst, tpoff);
}

// The jit/TargetFiber.h contract names, statically bound to the ELFv1 pair:
// at -O2 these compile to direct tail-calls, no vtable dereference on the
// context-switch path.
void fiberSwitchAsm(void **saveSp, void *newSp)
{
	fiberSwitchElfV1(saveSp, newSp);
}

void *fiberTargetPrimeStack(void *top, void (*entry)(void))
{
	return fiberPrimeStackElfV1(top, entry);
}
