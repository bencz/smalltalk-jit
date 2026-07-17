// Foreign-host binding for the one arch-only symbol the ELFv2 instance
// references: the fiber SWITCH is real POWER asm (FiberElfV2.c) and cannot
// exist in an x86 binary, but the golden harness links the full
// AbiPpc64leElfV2 instance, so its .fiberSwitch pointer lands here and traps
// loudly if anything ever tries to RUN it. Golden tests only EMIT bytes.
// Never wired into a ppc64le build (FiberElfV2.c provides the real symbol).
#ifdef __powerpc64__
#error "host stub must not be linked into a ppc64le build - FiberElfV2.c provides the real switch"
#endif

#include "vm/jit/ppc64/abi/elfv2/FiberElfV2.h"
#include "vm/core/Assert.h"

void fiberSwitchElfV2(void **saveSp, void *newSp)
{
	(void) saveSp; (void) newSp;
	FAIL();
}
