// Binds the GENERIC ABI names to the SysV instance. This is the only file
// that may define gX64Abi (and, once the fiber moves here, the TargetFiber.h
// symbols) — CMake's ST_ABI links exactly one Abi<Abi>Bind.c, which is what
// lets a test binary link SEVERAL instance files without symbol clashes.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/Abi.h"
#include "jit/x64/abi/sysv/FiberSysV.h"
#include "jit/TargetFiber.h"

const X64Abi *const gX64Abi = &AbiX64SysV;

// The jit/TargetFiber.h contract names, statically bound to the SysV pair:
// at -O2 these compile to direct tail-calls — no vtable dereference on the
// context-switch path.
void fiberSwitchAsm(void **saveSp, void *newSp)
{
	fiberSwitchSysV(saveSp, newSp);
}

void *fiberTargetPrimeStack(void *top, void (*entry)(void))
{
	return fiberPrimeStackSysV(top, entry);
}
