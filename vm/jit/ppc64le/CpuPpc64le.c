// The ppc64le CPU-model DECODE: pure functions over the kernel's hwcap words,
// plus the process-wide feature struct. HOST-INDEPENDENT by design (no
// syscalls, no getauxval, no POWER intrinsics), so the x86 dev host links this
// TU into its own `st` and the golden checks the bit decode against fabricated
// hwcaps: see vm/tests/EmitGoldenPpc64le.c. The one arch-only piece, the
// getauxval() call, lives in CpuDetectPpc64le.c.
//
// Contract and rationale: jit/TargetCpu.h. Nothing here is consulted by the
// code generator YET; this is the axis, not an optimization.
#include "jit/ppc64le/Cpu.h"
#include <string.h>

// The BASELINE: what may be assumed with no detection at all. It is HIGHER than
// the BE backend's, and legitimately so: ppc64le requires POWER8 by definition
// of the architecture, and the LE cross gcc agrees by defaulting to
// `.machine power8` (the BE one defaults to `.machine power4`). So ISA 2.07,
// AltiVec and VSX are floors here, not features.
//
// That same fact is behind the v20-v31 fiber-switch hazard: on LE the compiler
// really can hand us nonvolatile vector registers, so the gap is not
// theoretical here (vm/jit/ppc64le/DESIGN.md item 6).
Ppc64leCpu gPpc64leCpu = {
	.name = PPC64LE_CPU_BASELINE_NAME,
	.hasAltivec = 1,
	.hasVsx = 1,
	.hasGprVsrMoves = 1,
	.hwcap = PPC64LE_FEATURE_64 | PPC64LE_FEATURE_HAS_ALTIVEC
		| PPC64LE_FEATURE_ARCH_2_06 | PPC64LE_FEATURE_HAS_VSX,
	.hwcap2 = PPC64LE_FEATURE2_ARCH_2_07,
};

void ppc64leCpuDecode(Ppc64leCpu *cpu, uint64_t hwcap, uint64_t hwcap2)
{
	memset(cpu, 0, sizeof(*cpu));
	cpu->hwcap = hwcap;
	cpu->hwcap2 = hwcap2;

	cpu->hasAltivec = (hwcap & PPC64LE_FEATURE_HAS_ALTIVEC) != 0;
	cpu->hasVsx = (hwcap & PPC64LE_FEATURE_HAS_VSX) != 0;

	// Levels are made CUMULATIVE here rather than trusting the reporter to set
	// every lower bit. Real kernels do, but qemu-user is measurably stingier
	// than a kernel about the lower bits, and an emit site asking "isPower9?"
	// must never depend on the reporter's generosity.
	cpu->isPower10 = (hwcap2 & PPC64LE_FEATURE2_ARCH_3_1) != 0;
	cpu->isPower9 = cpu->isPower10 || (hwcap2 & PPC64LE_FEATURE2_ARCH_3_00) != 0;

	// Derived capability, from the words actually read (a decode never
	// inherits the floor; only the baseline GLOBAL assumes it, see below).
	// ISA 2.07 says mtvsrd/mfvsrd EXIST; the VSX facility bit says the OS
	// makes the register state available: both are required, exactly like
	// the BE decode. A kernel with VSX disabled clears the facility bit and
	// the moves would trap despite the ISA level.
	cpu->hasGprVsrMoves = (cpu->isPower9 || (hwcap2 & PPC64LE_FEATURE2_ARCH_2_07) != 0)
		&& cpu->hasVsx;

	// Best-effort model name, for humans only: never branch the codegen on it.
	// AT_PLATFORM would be authoritative but is NOT available everywhere
	// (measured: qemu-user returns NULL for it on every CPU model), so the name
	// is inferred from the same bits the features come from. Anything that
	// reports less than POWER8 is not a valid ppc64le host, so it is named for
	// what it claims rather than silently promoted.
	cpu->name = cpu->isPower10 ? "power10"
		: cpu->isPower9 ? "power9"
		: (hwcap2 & PPC64LE_FEATURE2_ARCH_2_07) ? "power8"
		: "under-reported";
}

// ST_CPU=<name>: synthesize the hwcaps a real chip of that generation reports
// and run them through the SAME decode, so a forced set can never disagree with
// a detected one. This is how a POWER10 path gets emitted and inspected from an
// x86 dev box.
//
// Ordered oldest to newest, each row CUMULATIVE over the previous, which is the
// property the golden asserts.
static const struct {
	const char *name;
	uint64_t hwcap;
	uint64_t hwcap2;
} Ppc64leCpuModels[] = {
	{ "power8",  PPC64LE_FEATURE_64 | PPC64LE_FEATURE_HAS_ALTIVEC
	             | PPC64LE_FEATURE_ARCH_2_06 | PPC64LE_FEATURE_HAS_VSX,
	             PPC64LE_FEATURE2_ARCH_2_07 },
	{ "power9",  PPC64LE_FEATURE_64 | PPC64LE_FEATURE_HAS_ALTIVEC
	             | PPC64LE_FEATURE_ARCH_2_06 | PPC64LE_FEATURE_HAS_VSX,
	             PPC64LE_FEATURE2_ARCH_2_07 | PPC64LE_FEATURE2_ARCH_3_00
	             | PPC64LE_FEATURE2_DARN },
	{ "power10", PPC64LE_FEATURE_64 | PPC64LE_FEATURE_HAS_ALTIVEC
	             | PPC64LE_FEATURE_ARCH_2_06 | PPC64LE_FEATURE_HAS_VSX,
	             PPC64LE_FEATURE2_ARCH_2_07 | PPC64LE_FEATURE2_ARCH_3_00
	             | PPC64LE_FEATURE2_DARN | PPC64LE_FEATURE2_ARCH_3_1
	             | PPC64LE_FEATURE2_MMA },
	{ NULL, 0, 0 },
};

const char *const Ppc64leCpuNames[] = {
	"power8", "power9", "power10", NULL,
};

_Bool ppc64leCpuByName(Ppc64leCpu *cpu, const char *name)
{
	for (size_t i = 0; Ppc64leCpuModels[i].name != NULL; i++) {
		if (strcmp(name, Ppc64leCpuModels[i].name) == 0) {
			ppc64leCpuDecode(cpu, Ppc64leCpuModels[i].hwcap, Ppc64leCpuModels[i].hwcap2);
			return 1;
		}
	}
	return 0;
}
