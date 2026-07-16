#ifndef PPC64_CPU_H
#define PPC64_CPU_H

// The ppc64 (big-endian) CPU-MODEL binding: which POWER generation this
// process is running on. See jit/TargetCpu.h for the axis and its contract;
// ppc64le has its own copy, like every other part of these two backends.
//
// SPLIT, mirroring the ABI instance/Bind split:
//   this header + CpuPpc64.c  the feature struct and the pure DECODE of the
//                             kernel's hwcap words: host-independent, so the
//                             x86 dev host links it and the golden checks the
//                             decode against pinned fake hwcaps
//   CpuDetectPpc64.c          the actual getauxval() call: ppc64-only, a thin
//                             shim over the decode
// The bug class this split exists to catch is "read the wrong bit", which no
// end-to-end test can see (a wrong feature flag just silently loses or, worse,
// wrongly enables an instruction).
#include <stdint.h>

// Kernel AT_HWCAP/AT_HWCAP2 bits, copied from the ppc64 glibc's
// <bits/hwcap.h>. They are FROZEN kernel ABI, so a private copy is safe, and
// it is what keeps the decode compilable on a foreign host (x86's glibc has no
// PPC_FEATURE2_*). CpuDetectPpc64.c static_asserts this copy against the real
// header on every native build, so the two can never drift.
// (#define rather than an enum: ARCH_2_07 is 0x80000000, which does not fit
// the int an enumerator is restricted to before C23, and this build is
// -pedantic -Werror.)
#define PPC64_FEATURE_64 0x40000000u  // AT_HWCAP, 64-bit mode
#define PPC64_FEATURE_HAS_ALTIVEC 0x10000000u  // AT_HWCAP
#define PPC64_FEATURE_POWER4 0x00080000u  // AT_HWCAP, ISA 2.00
#define PPC64_FEATURE_POWER5 0x00040000u  // AT_HWCAP, ISA 2.02
#define PPC64_FEATURE_POWER5_PLUS 0x00020000u  // AT_HWCAP, ISA 2.03
#define PPC64_FEATURE_ARCH_2_05 0x00001000u  // AT_HWCAP, ISA 2.05 (POWER6)
#define PPC64_FEATURE_HAS_DFP 0x00000400u  // AT_HWCAP, POWER6 decimal FP
#define PPC64_FEATURE_ARCH_2_06 0x00000100u  // AT_HWCAP, ISA 2.06 (POWER7)
#define PPC64_FEATURE_HAS_VSX 0x00000080u  // AT_HWCAP
#define PPC64_FEATURE2_ARCH_2_07 0x80000000u  // AT_HWCAP2, ISA 2.07 (POWER8)
#define PPC64_FEATURE2_ARCH_3_00 0x00800000u  // AT_HWCAP2, ISA 3.0  (POWER9)
#define PPC64_FEATURE2_DARN 0x00200000u  // AT_HWCAP2, POWER9 darn
#define PPC64_FEATURE2_ARCH_3_1 0x00040000u  // AT_HWCAP2, ISA 3.1  (POWER10)
#define PPC64_FEATURE2_MMA 0x00020000u  // AT_HWCAP2, POWER10 MMA

// This backend's floor is the PowerPC 64-bit BASE ISA, and that is a measured
// fact, not an aspiration: every instruction the assembler can emit (all 186 in
// the golden's reference .s) is accepted by binutils at `-mppc64`, producing
// bytes identical to the default build. So the JIT runs on ANY 64-bit PowerPC,
// including a PowerPC 970 (Apple G5) and a POWER5. Keep it that way: anything
// gated on a flag below must have a base-ISA fallback.
//
// An Apple G5 running Debian ppc64 is a REAL, supported target, not a
// hypothetical: detection reports it as "ppc970" with altivec=1, vsx=0.
// (Do NOT try to verify that with qemu-user's 970 model: under it even a static
// pthread hello-world containing none of our code segfaults, which contradicts
// the real hardware. It is an emulation artifact; use a G5 or qemu-system.)
typedef struct {
	const char *name;      // "ppc970", "power8", ... : ST_CPU_INFO / ST_CPU

	// ISA levels, CUMULATIVE: a POWER10 sets every lower level too, so an emit
	// site can test exactly the level it needs.
	_Bool isPower6;        // ISA 2.05
	_Bool isPower7;        // ISA 2.06: VSX
	_Bool isPower8;        // ISA 2.07
	_Bool isPower9;        // ISA 3.0:  modsd/modud (our IntMod is divd+mulld+subf), darn
	_Bool isPower10;       // ISA 3.1:  prefixed insns (pli/paddi/pld)

	// INDEPENDENT of the ISA level, and the G5 is exactly why: a PowerPC 970
	// has AltiVec but is POWER4-class, while a POWER5 is NEWER and has none.
	// Never infer one from the other.
	_Bool hasAltivec;
	_Bool hasVsx;          // POWER7+; also gates the v20-v31 fiber-switch decision

	uint64_t hwcap;        // raw, for ST_CPU_INFO and post-mortems
	uint64_t hwcap2;
} Ppc64Cpu;

// Read-only after targetCpuDetect(); defined in CpuPpc64.c at the BASELINE.
extern Ppc64Cpu gPpc64Cpu;

// Pure decode of the kernel's two hwcap words into `cpu`. No syscalls, no
// globals: callable on any host with fabricated inputs, which is how the
// golden tests it.
void ppc64CpuDecode(Ppc64Cpu *cpu, uint64_t hwcap, uint64_t hwcap2);

// Force a named feature set (ST_CPU=<name>). Returns 0 if the name is unknown.
// Also the mechanism the golden uses to pin a deterministic feature set.
_Bool ppc64CpuByName(Ppc64Cpu *cpu, const char *name);

// The names ppc64CpuByName accepts, NULL-terminated (for ST_CPU=? and tests).
extern const char *const Ppc64CpuNames[];

// The safe default when detection is unavailable or under-reports: claim
// NOTHING, which is the PowerPC 64-bit base ISA the JIT is measured to emit
// (see the note above the struct). It is also what an Apple G5 runs.
#define PPC64_CPU_BASELINE_NAME "baseline"

#endif
