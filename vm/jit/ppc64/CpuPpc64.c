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

// Private representation of the highest reported ISA level. Keeping the
// ordering in one place makes the public cumulative booleans mechanical and
// prevents a newly added level from accidentally skipping an older one.
typedef enum {
	PPC64_ISA_BASELINE = 0,
	PPC64_ISA_2_05,
	PPC64_ISA_2_06,
	PPC64_ISA_2_07,
	PPC64_ISA_3_00,
	PPC64_ISA_3_1,
} Ppc64IsaLevel;

static Ppc64IsaLevel highestIsaLevel(uint64_t hwcap, uint64_t hwcap2)
{
	if ((hwcap2 & PPC64_FEATURE2_ARCH_3_1) != 0)
		return PPC64_ISA_3_1;
	if ((hwcap2 & PPC64_FEATURE2_ARCH_3_00) != 0)
		return PPC64_ISA_3_00;
	if ((hwcap2 & PPC64_FEATURE2_ARCH_2_07) != 0)
		return PPC64_ISA_2_07;
	if ((hwcap & PPC64_FEATURE_ARCH_2_06) != 0)
		return PPC64_ISA_2_06;
	if ((hwcap & PPC64_FEATURE_ARCH_2_05) != 0)
		return PPC64_ISA_2_05;
	return PPC64_ISA_BASELINE;
}

void ppc64CpuDecode(Ppc64Cpu *cpu, uint64_t hwcap, uint64_t hwcap2)
{
	const Ppc64IsaLevel isa = highestIsaLevel(hwcap, hwcap2);

	memset(cpu, 0, sizeof(*cpu));
	cpu->hwcap = hwcap;
	cpu->hwcap2 = hwcap2;

	// Vector support is INDEPENDENT of the ISA level: a PowerPC 970 (G5) has
	// AltiVec at POWER4 level, and a POWER5 is newer with none at all. These bits
	// also describe whether the OS makes the corresponding register state usable.
	cpu->hasAltivec = (hwcap & PPC64_FEATURE_HAS_ALTIVEC) != 0;
	cpu->hasVsx = (hwcap & PPC64_FEATURE_HAS_VSX) != 0;

	// The public levels are cumulative even when a reporter supplies only its
	// highest architectural bit. This is deliberate for kernels, hypervisors and
	// qemu-user variants that omit lower-generation marker bits.
	cpu->isPower6 = isa >= PPC64_ISA_2_05;
	cpu->isPower7 = isa >= PPC64_ISA_2_06;
	cpu->isPower8 = isa >= PPC64_ISA_2_07;
	cpu->isPower9 = isa >= PPC64_ISA_3_00;
	cpu->isPower10 = isa >= PPC64_ISA_3_1;

	// mtvsrd/mfvsrd require both ISA 2.07 and usable VSX state. Named profiles
	// already contain both, so this only changes contradictory or under-reported
	// inputs, where conservatively losing the optimization is the safe outcome.
	cpu->hasGprVsrMoves = cpu->isPower8 && cpu->hasVsx;

	// Best-effort model name, for humans only: never branch the codegen on it.
	// AT_PLATFORM is not available through every supported OS/provider and
	// qemu-user may omit generation marker bits, so retain the historic AltiVec
	// fallback that identifies the supported G5 floor as ppc970.
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

// ST_CPU=<name> uses canonical synthetic profiles and runs them through the
// SAME decode, so a forced set cannot disagree with a detected one. The bit
// patterns are intentionally unchanged: existing goldens and forced-codegen
// inspection keep the same inputs. They are semantic test profiles, not a
// promise to reproduce every OS's auxv word byte-for-byte.
//
// ppc970 and power4 are sibling POWER4-class profiles because AltiVec is
// independent. From power4 through power10, the generation markers are kept
// cumulative for the existing golden property.
#define PPC64_CPU_MODEL_LIST(X) \
	X(PPC64_CPU_BASELINE_NAME, 0, 0) \
	X("ppc970", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_HAS_ALTIVEC, 0) \
	X("power4", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4, 0) \
	X("power5", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5, 0) \
	X("power5+", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS, 0) \
	X("power6", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS \
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP \
		| PPC64_FEATURE_HAS_ALTIVEC, 0) \
	X("power7", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS \
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP \
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06 \
		| PPC64_FEATURE_HAS_VSX, 0) \
	X("power8", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS \
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP \
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06 \
		| PPC64_FEATURE_HAS_VSX, PPC64_FEATURE2_ARCH_2_07) \
	X("power9", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS \
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP \
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06 \
		| PPC64_FEATURE_HAS_VSX, PPC64_FEATURE2_ARCH_2_07 \
		| PPC64_FEATURE2_ARCH_3_00 | PPC64_FEATURE2_DARN) \
	X("power10", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 \
		| PPC64_FEATURE_POWER5 | PPC64_FEATURE_POWER5_PLUS \
		| PPC64_FEATURE_ARCH_2_05 | PPC64_FEATURE_HAS_DFP \
		| PPC64_FEATURE_HAS_ALTIVEC | PPC64_FEATURE_ARCH_2_06 \
		| PPC64_FEATURE_HAS_VSX, PPC64_FEATURE2_ARCH_2_07 \
		| PPC64_FEATURE2_ARCH_3_00 | PPC64_FEATURE2_DARN \
		| PPC64_FEATURE2_ARCH_3_1 | PPC64_FEATURE2_MMA)

#define PPC64_CPU_MODEL_ROW(name_, hwcap_, hwcap2_) { name_, hwcap_, hwcap2_ },
static const struct {
	const char *name;
	uint64_t hwcap;
	uint64_t hwcap2;
} Ppc64CpuModels[] = {
	PPC64_CPU_MODEL_LIST(PPC64_CPU_MODEL_ROW)
	{ NULL, 0, 0 },
};
#undef PPC64_CPU_MODEL_ROW

#define PPC64_CPU_NAME_ROW(name_, hwcap_, hwcap2_) name_,
const char *const Ppc64CpuNames[] = {
	PPC64_CPU_MODEL_LIST(PPC64_CPU_NAME_ROW)
	NULL,
};
#undef PPC64_CPU_NAME_ROW

_Bool ppc64CpuByName(Ppc64Cpu *cpu, const char *name)
{
	if (cpu == NULL || name == NULL)
		return 0;

	for (size_t i = 0; Ppc64CpuModels[i].name != NULL; i++) {
		if (strcmp(name, Ppc64CpuModels[i].name) == 0) {
			ppc64CpuDecode(cpu, Ppc64CpuModels[i].hwcap, Ppc64CpuModels[i].hwcap2);
			return 1;
		}
	}
	return 0;
}

#undef PPC64_CPU_MODEL_LIST