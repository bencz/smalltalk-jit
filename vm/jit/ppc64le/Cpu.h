#ifndef PPC64LE_CPU_H
#define PPC64LE_CPU_H

// The ppc64le (little-endian) CPU-MODEL binding: which POWER generation this
// process is running on. See jit/TargetCpu.h for the axis and its contract;
// ppc64 (BE) has its own copy, like every other part of these two backends.
//
// It is deliberately NOT a mirror of the BE one, and the asymmetry is real
// rather than an oversight: ppc64le has no pre-POWER8 member (the ELFv2
// little-endian ecosystem starts at POWER8), so there is nothing below ISA 2.07
// to model here. The BE backend, by contrast, must reach all the way down to a
// PowerPC 970 (Apple G5) and a POWER5, where AltiVec presence does not follow
// the ISA level.
//
// SPLIT, mirroring the ABI instance/Bind split:
//   this header + CpuPpc64le.c  the feature struct and the pure DECODE of the
//                               kernel's hwcap words: host-independent, so the
//                               x86 dev host links it and the golden checks the
//                               decode against pinned fake hwcaps
//   CpuDetectPpc64le.c          the actual getauxval() call: ppc64le-only, a
//                               thin shim over the decode
// The bug class this split exists to catch is "read the wrong bit", which no
// end-to-end test can see (a wrong feature flag just silently loses or, worse,
// wrongly enables an instruction).
#include <stdint.h>

// Kernel AT_HWCAP/AT_HWCAP2 bits, copied from the ppc64 glibc's
// <bits/hwcap.h>. They are FROZEN kernel ABI, so a private copy is safe, and it
// is what keeps the decode compilable on a foreign host (x86's glibc has no
// PPC_FEATURE2_*). CpuDetectPpc64le.c static_asserts this copy against the real
// header on every native build, so the two can never drift.
// (#define rather than an enum: ARCH_2_07 is 0x80000000, which does not fit
// the int an enumerator is restricted to before C23, and this build is
// -pedantic -Werror.)
#define PPC64LE_FEATURE_64 0x40000000u  // AT_HWCAP, 64-bit mode
#define PPC64LE_FEATURE_HAS_ALTIVEC 0x10000000u  // AT_HWCAP
#define PPC64LE_FEATURE_ARCH_2_06 0x00000100u  // AT_HWCAP, ISA 2.06 (POWER7)
#define PPC64LE_FEATURE_HAS_VSX 0x00000080u  // AT_HWCAP
#define PPC64LE_FEATURE2_ARCH_2_07 0x80000000u  // AT_HWCAP2, ISA 2.07 (POWER8)
#define PPC64LE_FEATURE2_ARCH_3_00 0x00800000u  // AT_HWCAP2, ISA 3.0  (POWER9)
#define PPC64LE_FEATURE2_DARN 0x00200000u  // AT_HWCAP2, POWER9 darn
#define PPC64LE_FEATURE2_ARCH_3_1 0x00040000u  // AT_HWCAP2, ISA 3.1  (POWER10)
#define PPC64LE_FEATURE2_MMA 0x00020000u  // AT_HWCAP2, POWER10 MMA

typedef struct {
	const char *name;      // "power8", "power9", ... : ST_CPU_INFO / ST_CPU

	// ISA levels above the POWER8 floor, CUMULATIVE: a POWER10 sets isPower9
	// too, so an emit site can test exactly the level it needs. There is no
	// isPower8: on this backend it is always true (see CpuPpc64le.c).
	_Bool isPower9;        // ISA 3.0:  modsd/modud (our IntMod is divd+mulld+subf), darn
	_Bool isPower10;       // ISA 3.1:  prefixed insns (pli/paddi/pld)

	// Always true on a real ppc64le, kept explicit so an emit site reads the
	// same on both POWER backends and so an under-reporting kernel or emulator
	// is visible rather than silently assumed away.
	_Bool hasAltivec;
	_Bool hasVsx;          // also gates the v20-v31 fiber-switch decision

	uint64_t hwcap;        // raw, for ST_CPU_INFO and post-mortems
	uint64_t hwcap2;
} Ppc64leCpu;

// Read-only after targetCpuDetect(); defined in CpuPpc64le.c at the BASELINE.
extern Ppc64leCpu gPpc64leCpu;

// Pure decode of the kernel's two hwcap words into `cpu`. No syscalls, no
// globals: callable on any host with fabricated inputs, which is how the golden
// tests it.
void ppc64leCpuDecode(Ppc64leCpu *cpu, uint64_t hwcap, uint64_t hwcap2);

// Force a named feature set (ST_CPU=<name>). Returns 0 if the name is unknown.
// Also the mechanism the golden uses to pin a deterministic feature set.
_Bool ppc64leCpuByName(Ppc64leCpu *cpu, const char *name);

// The names ppc64leCpuByName accepts, NULL-terminated (for ST_CPU=? and tests).
extern const char *const Ppc64leCpuNames[];

// The floor of the ARCHITECTURE, not merely of this build: ppc64le has no
// pre-POWER8 member, and the LE cross gcc confirms it by defaulting to
// `.machine power8` where the BE one defaults to `.machine power4`.
#define PPC64LE_CPU_BASELINE_NAME "power8"

#endif
