// Foreign-host binding for the one arch-only symbol the ELFv1 instance
// references: the fiber SWITCH is real POWER asm (FiberElfV1.c) and cannot
// exist in an x86 binary, but the golden harness links the full
// AbiPpc64ElfV1 instance — so its .fiberSwitch pointer lands here and traps
// loudly if anything ever tries to RUN it. Golden tests only EMIT bytes.
// Never wired into a ppc64 build (FiberElfV1.c provides the real symbol).
#ifdef __powerpc64__
#error "host stub must not be linked into a ppc64 build - FiberElfV1.c provides the real switch"
#endif

#include "vm/jit/ppc64/abi/elfv1/FiberElfV1.h"
#include "vm/core/Assert.h"

void fiberSwitchElfV1(void **saveSp, void *newSp)
{
	(void) saveSp; (void) newSp;
	FAIL();
}
