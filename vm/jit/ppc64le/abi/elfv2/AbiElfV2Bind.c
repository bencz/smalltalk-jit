// ppc64le ELFv2 ABI binding SKELETON. When the real port begins this file
// mirrors vm/jit/x64/abi/sysv/AbiSysVBind.c: it will define gPpc64Abi (a
// Ppc64Abi ops-struct mirroring X64Abi) plus the real fiber pair. Until then
// the generic contract names are FAIL() stubs so the tree links and every
// non-JIT test runs; the first fiber spawn dies loudly here.
//
// ELFv1 (big-endian/AIX heritage) specifics to implement here: function
// descriptors (.opd: {entry, TOC, environ}) — calling a C function pointer
// means loading the entry AND r2 from the descriptor; TOC save/restore around
// cross-module calls. ELFv2 (little-endian) uses local/global entry points
// (localentry) instead. See PORTING.md.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vm/jit/ppc64le/ is LITTLE-ENDIAN ppc64 only (big-endian ppc64 has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/TargetFiber.h"
#include "jit/ppc64le/AssemblerPpc64le.h"
#include "core/Assert.h"

void fiberSwitchAsm(void **saveSp, void *newSp)
{
	(void) saveSp; (void) newSp;
	FAIL(); // PORT_ME: ppc64 elfv2 fiber switch (no push/pop - stdu/ld frames)
}

void *fiberTargetPrimeStack(void *top, void (*entry)(void))
{
	(void) top; (void) entry;
	FAIL();
	return NULL;
}

void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff)
{
	(void) buffer; (void) dst; (void) tpoff;
	FAIL(); // PORT_ME(tls): r13-relative load with the 0x7000 bias
}
