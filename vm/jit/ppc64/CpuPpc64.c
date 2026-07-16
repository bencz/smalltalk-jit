// The ppc64 CPU-model DECODE: pure functions over the kernel's hwcap words,
// plus the process-wide feature struct. HOST-INDEPENDENT by design (no
// syscalls, no getauxval, no POWER intrinsics), so the x86 dev host links this
// TU into its own `st` and the golden checks the bit decode against fabricated
// hwcaps: see vm/tests/EmitGoldenPpc64.c. The one arch-only piece, the
// getauxval() call, lives in CpuDetectPpc64.c.
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

	// Vector support is INDEPENDENT of the ISA level: a PowerPC 970 (G5) has
	// AltiVec at POWER4 level, and a POWER5 is newer with none at all.
	cpu->hasAltivec = (hwcap & PPC64_FEATURE_HAS_ALTIVEC) != 0;
	cpu->hasVsx = (hwcap & PPC64_FEATURE_HAS_VSX) != 0;

	// Levels are made CUMULATIVE here rather than trusting the reporter to set
	// every lower bit. Real kernels do, but qemu-user does NOT: emulating a 970
	// it reports neither PPC_FEATURE_POWER4 nor ARCH_2_05, only 64|ALTIVEC|FPU
	// (measured). An emit site asking "isPower7?" must never depend on the
	// reporter's generosity.
	cpu->isPower10 = (hwcap2 & PPC64_FEATURE2_ARCH_3_1) != 0;
	cpu->isPower9 = cpu->isPower10 || (hwcap2 & PPC64_FEATURE2_ARCH_3_00) != 0;
	cpu->isPower8 = cpu->isPower9 || (hwcap2 & PPC64_FEATURE2_ARCH_2_07) != 0;
	cpu->isPower7 = cpu->isPower8 || (hwcap & PPC64_FEATURE_ARCH_2_06) != 0;
	cpu->isPower6 = cpu->isPower7 || (hwcap & PPC64_FEATURE_ARCH_2_05) != 0;

	// Best-effort model name, for humans only: never branch the codegen on it.
	// AT_PLATFORM would be the authoritative source, but it is NOT available
	// everywhere (measured: qemu-user returns NULL for it on every CPU model),
	// so the name is inferred from the same bits the features come from.
	cpu->name = cpu->isPower10 ? "power10"
		: cpu->isPower9 ? "power9"
		: cpu->isPower8 ? "power8"
		: cpu->isPower7 ? "power7"
		: cpu->isPower6 ? "power6"
		: (hwcap & PPC64_FEATURE_POWER5_PLUS) ? "power5+"
		: (hwcap & PPC64_FEATURE_POWER5) ? "power5"
		// A 970 (G5) and a POWER4 are the same ISA level; AltiVec is what tells
		// them apart, and it is the only signal qemu leaves us.
		: cpu->hasAltivec ? "ppc970"
		: (hwcap & PPC64_FEATURE_POWER4) ? "power4"
		: PPC64_CPU_BASELINE_NAME;
}

// ST_CPU=<name>: synthesize the hwcaps a real chip of that generation reports
// and run them through the SAME decode, so a forced set can never disagree with
// a detected one. This is how a POWER10 path gets emitted and inspected from an
// x86 dev box, and how the G5 floor stays testable without a G5.
//
// The table is deliberately ordered oldest to newest, and each row is
// CUMULATIVE over the previous, which is the property the golden asserts.
static const struct {
	const char *name;
	uint64_t hwcap;
	uint64_t hwcap2;
} Ppc64CpuModels[] = {
	{ PPC64_CPU_BASELINE_NAME, 0, 0 },
	// A 970 (Apple G5) is POWER4-class WITH AltiVec.
	{ "ppc970",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_HAS_ALTIVEC, 0 },
	{ "power4",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4, 0 },
	// POWER5 is NEWER than the G5 and has NO AltiVec.
	{ "power5",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5, 0 },
	{ "power5+", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5
	             | PPC64_FEATURE_POWER5_PLUS, 0 },
	// AltiVec is back with POWER6, which also brings decimal FP.
	{ "power6",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5
	             | PPC64_FEATURE_POWER5_PLUS | PPC64_FEATURE_ARCH_2_05
	             | PPC64_FEATURE_HAS_DFP | PPC64_FEATURE_HAS_ALTIVEC, 0 },
	{ "power7",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5
	             | PPC64_FEATURE_POWER5_PLUS | PPC64_FEATURE_ARCH_2_05
	             | PPC64_FEATURE_HAS_DFP | PPC64_FEATURE_HAS_ALTIVEC
	             | PPC64_FEATURE_ARCH_2_06 | PPC64_FEATURE_HAS_VSX, 0 },
	{ "power8",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5
	             | PPC64_FEATURE_POWER5_PLUS | PPC64_FEATURE_ARCH_2_05
	             | PPC64_FEATURE_HAS_DFP | PPC64_FEATURE_HAS_ALTIVEC
	             | PPC64_FEATURE_ARCH_2_06 | PPC64_FEATURE_HAS_VSX,
	             PPC64_FEATURE2_ARCH_2_07 },
	{ "power9",  PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5
	             | PPC64_FEATURE_POWER5_PLUS | PPC64_FEATURE_ARCH_2_05
	             | PPC64_FEATURE_HAS_DFP | PPC64_FEATURE_HAS_ALTIVEC
	             | PPC64_FEATURE_ARCH_2_06 | PPC64_FEATURE_HAS_VSX,
	             PPC64_FEATURE2_ARCH_2_07 | PPC64_FEATURE2_ARCH_3_00
	             | PPC64_FEATURE2_DARN },
	{ "power10", PPC64_FEATURE_64 | PPC64_FEATURE_POWER4 | PPC64_FEATURE_POWER5
	             | PPC64_FEATURE_POWER5_PLUS | PPC64_FEATURE_ARCH_2_05
	             | PPC64_FEATURE_HAS_DFP | PPC64_FEATURE_HAS_ALTIVEC
	             | PPC64_FEATURE_ARCH_2_06 | PPC64_FEATURE_HAS_VSX,
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
			ppc64CpuDecode(cpu, Ppc64CpuModels[i].hwcap, Ppc64CpuModels[i].hwcap2);
			return 1;
		}
	}
	return 0;
}
