// The ppc64 CPU-model DECODE: pure functions over the kernel's hwcap words,
// plus the process-wide feature struct. HOST-INDEPENDENT by design (no
// syscalls, no getauxval, no POWER intrinsics), so the x86 dev host links this
// TU into its own `st` and the golden checks the bit decode against fabricated
// hwcaps: see vm/tests/EmitGoldenPpc64.c. The arch-only binding lives in
// CpuDetectPpc64.c; acquisition of the words lives behind osCpuFeatureWords().
//
// Contract and rationale: jit/TargetCpu.h. Nothing here is consulted by the
// code generator YET; this is the axis, not an optimization.
#include "jit/ppc64/Cpu.h"
#include <string.h>

// The BASELINE: the PowerPC 64-bit base ISA, which is what the JIT is measured
// to emit (see Cpu.h). Claiming nothing means a failed, absent or
// under-reporting detection can only lose optimizations, never emit an illegal
// instruction, and it is the level an Apple G5 runs.
Ppc64Cpu gPpc64Cpu = {
	.name = PPC64_CPU_BASELINE_NAME,
};

void ppc64CpuDecode(Ppc64Cpu *cpu, uint64_t hwcap, uint64_t hwcap2)
{
	memset(cpu, 0, sizeof(*cpu));
	cpu->hwcap = hwcap;
	cpu->hwcap2 = hwcap2;

	// Facilities remain independent of the inferred ISA level. This preserves
	// the POWER4-class G5 with AltiVec and the newer POWER5 without AltiVec.
	cpu->hasAltivec = (hwcap & PPC64_FEATURE_HAS_ALTIVEC) != 0;
	cpu->hasVsx = (hwcap & PPC64_FEATURE_HAS_VSX) != 0;

	// Normalize only the ordered ISA levels. A reporter may provide just its
	// highest architecture bit, so each higher level implies every lower level.
	cpu->isPower10 = (hwcap2 & PPC64_FEATURE2_ARCH_3_1) != 0;
	cpu->isPower9 = cpu->isPower10
		|| (hwcap2 & PPC64_FEATURE2_ARCH_3_00) != 0;
	cpu->isPower8 = cpu->isPower9
		|| (hwcap2 & PPC64_FEATURE2_ARCH_2_07) != 0;
	cpu->isPower7 = cpu->isPower8
		|| (hwcap & PPC64_FEATURE_ARCH_2_06) != 0;
	cpu->isPower6 = cpu->isPower7
		|| (hwcap & PPC64_FEATURE_ARCH_2_05) != 0;

	// Do not manufacture an independent facility from an ISA marker. Named CPU
	// profiles contain both bits, so their observable behavior is unchanged.
	cpu->hasGprVsrMoves = cpu->isPower8 && cpu->hasVsx;

	// Best-effort model name, for humans only: never branch the codegen on it.
	// Keep the AltiVec fallback because qemu-user's 970 model may omit POWER4.
	cpu->name = cpu->isPower10 ? "power10"
		: cpu->isPower9 ? "power9"
		: cpu->isPower8 ? "power8"
		: cpu->isPower7 ? "power7"
		: cpu->isPower6 ? "power6"
		: (hwcap & PPC64_FEATURE_POWER5_PLUS) ? "power5+"
		: (hwcap & PPC64_FEATURE_POWER5) ? "power5"
		: cpu->hasAltivec ? "ppc970"
		: (hwcap & PPC64_FEATURE_POWER4) ? "power4"
		: PPC64_CPU_BASELINE_NAME;
}

// ST_CPU=<name> uses canonical synthetic profiles and passes them through the
// same decoder. The bit patterns and accepted names remain exactly as before.
// ppc970 and power4 are sibling POWER4-class profiles because AltiVec is an
// independent facility, so the complete table is not monotonic bit-for-bit.
static const struct {
	const char *name;
	uint64_t hwcap;
	uint64_t hwcap2;
} Ppc64CpuModels[] = {
	{ PPC64_CPU_BASELINE_NAME, 0, 0 },
	{ "ppc970", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_HAS_ALTIVEC, 0 },
	{ "power4", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4, 0 },
	{ "power5", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5, 0 },
	{ "power5+", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS, 0 },
	{ "power6", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP
		| PPC64_FEATURE_HAS_ALTIVEC, 0 },
	{ "power7", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06
		| PPC64_FEATURE_HAS_VSX, 0 },
	{ "power8", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06
		| PPC64_FEATURE_HAS_VSX,
		PPC64_FEATURE2_ARCH_2_07 },
	{ "power9", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06
		| PPC64_FEATURE_HAS_VSX,
		PPC64_FEATURE2_ARCH_2_07 | PPC64_FEATURE2_ARCH_3_00
		| PPC64_FEATURE2_DARN },
	{ "power10", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06
		| PPC64_FEATURE_HAS_VSX,
		PPC64_FEATURE2_ARCH_2_07 | PPC64_FEATURE2_ARCH_3_00
		| PPC64_FEATURE2_DARN | PPC64_FEATURE2_ARCH_3_1
		| PPC64_FEATURE2_MMA },
	{ NULL, 0, 0 },
};

const char *const Ppc64CpuNames[] = {
	PPC64_CPU_BASELINE_NAME, "ppc970", "power4", "power5", "power5+",
	"power6", "power7", "power8", "power9", "power10", NULL,
};

_Bool ppc64CpuByName(Ppc64Cpu *cpu, const char *name)
{
	for (size_t i = 0; Ppc64CpuModels[i].name != NULL; i++) {
		if (strcmp(name, Ppc64CpuModels[i].name) == 0) {
			ppc64CpuDecode(cpu, Ppc64CpuModels[i].hwcap,
				Ppc64CpuModels[i].hwcap2);
			return 1;
		}
	}
	return 0;
}
