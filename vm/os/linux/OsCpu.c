// Linux CPU topology / scheduling services and CPU-feature discovery
// (vm/os/Os.h).
#define _GNU_SOURCE // sched_getaffinity / CPU_COUNT
#include "os/Os.h"
#include <sched.h>
#include <unistd.h>
#include <sys/auxv.h>

// The ELF auxiliary vector is a LINUX/ELF facility, not an architecture one,
// which is exactly why this lives on the OS axis: AIX has no auxv (it answers
// through _system_configuration.implementation instead), FreeBSD spells it
// elf_aux_info(), and macOS uses sysctl. The BITS carried in these words are
// architectural, and their meaning is defined by vm/jit/<arch>/Cpu.h.
//
// AT_HWCAP2 is not defined on every arch/libc combination; falling back to 0 is
// safe, since the arch decode then simply claims less.
#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif


int osAvailableCoreCount(void)
{
#ifdef CPU_COUNT
	cpu_set_t set;
	if (sched_getaffinity(0, sizeof(set), &set) == 0) {
		int n = CPU_COUNT(&set);
		if (n >= 1) {
			return n;
		}
	}
#endif
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n >= 1 ? (int) n : 1;
}


// Pin the arch's private copy of the kernel ABI bits against THIS platform's
// real header. The copy exists so the decode compiles on a foreign host; the
// assert exists so it can never drift. It belongs here, on the OS axis, because
// <bits/hwcap.h> is Linux's header, not the architecture's.
#ifdef __powerpc64__
#include "jit/ppc64/Cpu.h"
#define ST_PPC_CPU_BITS(p) \
	_Static_assert(p##_FEATURE_HAS_ALTIVEC == PPC_FEATURE_HAS_ALTIVEC, "hwcap copy drifted"); \
	_Static_assert(p##_FEATURE_ARCH_2_06 == PPC_FEATURE_ARCH_2_06, "hwcap copy drifted"); \
	_Static_assert(p##_FEATURE_HAS_VSX == PPC_FEATURE_HAS_VSX, "hwcap copy drifted"); \
	_Static_assert(p##_FEATURE2_ARCH_2_07 == PPC_FEATURE2_ARCH_2_07, "hwcap2 copy drifted"); \
	_Static_assert(p##_FEATURE2_ARCH_3_00 == PPC_FEATURE2_ARCH_3_00, "hwcap2 copy drifted"); \
	_Static_assert(p##_FEATURE2_ARCH_3_1 == PPC_FEATURE2_ARCH_3_1, "hwcap2 copy drifted")
ST_PPC_CPU_BITS(PPC64);
_Static_assert(PPC64_FEATURE_64 == PPC_FEATURE_64, "hwcap copy drifted");
_Static_assert(PPC64_FEATURE_POWER4 == PPC_FEATURE_POWER4, "hwcap copy drifted");
_Static_assert(PPC64_FEATURE_POWER5 == PPC_FEATURE_POWER5, "hwcap copy drifted");
_Static_assert(PPC64_FEATURE_POWER5_PLUS == PPC_FEATURE_POWER5_PLUS, "hwcap copy drifted");
_Static_assert(PPC64_FEATURE_ARCH_2_05 == PPC_FEATURE_ARCH_2_05, "hwcap copy drifted");
_Static_assert(PPC64_FEATURE_HAS_DFP == PPC_FEATURE_HAS_DFP, "hwcap copy drifted");
_Static_assert(PPC64_FEATURE2_DARN == PPC_FEATURE2_DARN, "hwcap2 copy drifted");
_Static_assert(PPC64_FEATURE2_MMA == PPC_FEATURE2_MMA, "hwcap2 copy drifted");
#endif


_Bool osCpuFeatureWords(uint64_t *word0, uint64_t *word1)
{
	*word0 = (uint64_t) getauxval(AT_HWCAP);
	*word1 = (uint64_t) getauxval(AT_HWCAP2);
	return 1;
}
