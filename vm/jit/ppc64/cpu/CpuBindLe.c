// The little-endian CPU-model bind: the process-wide feature struct at ITS
// baseline, plus the ST_CPU names this target accepts. Picked by CMake next
// to the ABI bind (elfv2); the pure decode in CpuPpc64.c is shared by both
// targets and by the host goldens, which link NO bind.
//
// The LE BASELINE is the floor of the ARCHITECTURE, not merely of a build:
// ppc64le has no pre-POWER8 member, and the LE cross gcc confirms it by
// defaulting to `.machine power8` where the BE one defaults to
// `.machine power4`. So ISA 2.07, AltiVec and VSX are floors here, not
// features, and the cumulative level booleans start true. A DECODE never
// inherits this floor (it claims only what it read); only this global
// assumes it.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "CpuBindLe.c is for LITTLE-ENDIAN ppc64 builds - check the CPU bind selection in CMakeLists.txt"
#endif

#include "jit/ppc64/Cpu.h"
#include <stddef.h>

Ppc64Cpu gPpc64Cpu = {
	.name = "power8",
	.isPower6 = 1,
	.isPower7 = 1,
	.isPower8 = 1,
	.hasAltivec = 1,
	.hasVsx = 1,
	.hasGprVsrMoves = 1,
	.hwcap = PPC64_FEATURE_64 | PPC64_FEATURE_HAS_ALTIVEC
		| PPC64_FEATURE_ARCH_2_06 | PPC64_FEATURE_HAS_VSX,
	.hwcap2 = PPC64_FEATURE2_ARCH_2_07,
};

// Anything below the POWER8 floor is not a valid ppc64le host: ST_CPU
// rejects it here rather than silently emitting for a machine that cannot
// exist.
static const char *const LeAccepted[] = {
	"power8", "power9", "power10", NULL,
};
const char *const *const Ppc64CpuAccepted = LeAccepted;
