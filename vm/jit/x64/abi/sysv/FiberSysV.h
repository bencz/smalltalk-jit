#ifndef FIBER_SYSV_H
#define FIBER_SYSV_H

// SysV x86-64 stackful-coroutine pair (FiberSysV.c), under abi-unique names so
// several ABI instances can coexist in one test binary. Production reaches
// these through the jit/TargetFiber.h wrappers in AbiSysVBind.c.
void fiberSwitchSysV(void **saveSp, void *newSp);
void *fiberPrimeStackSysV(void *top, void (*entry)(void));

#endif
