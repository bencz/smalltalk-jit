#ifndef X64_CPU_H
#define X64_CPU_H

// The x64 CPU-MODEL binding: which x86-64 microarchitecture this process is
// running on. See jit/TargetCpu.h for the axis and its contract.
//
// x64 is the one backend where the CPUID equivalent is a plain unprivileged
// instruction, so detection reads CPUID directly rather than going through the
// kernel's AT_HWCAP. On POWER the same register exists (the PVR, SPR 287) but
// is supervisor-only, so userspace gets the answer from the kernel instead;
// that is why each backend owns its own detection.
//
// CPUID ALONE IS NOT ENOUGH, and this is the trap the split below exists for:
// for any feature with extra register state (AVX and up), the CPU may report
// the feature while the OS has not enabled the state, and executing it then
// faults. The OS's answer lives in XCR0, reached via XGETBV and only legal when
// CPUID says OSXSAVE. So the decode takes XCR0 as an input and refuses AVX-class
// features without it.
#include <stdint.h>

// CPUID feature bits, by (leaf, register). Kept explicit because the raw values
// COLLIDE across leaves: bit 5 is LZCNT in leaf 0x80000001 ECX, AVX2 in leaf 7
// EBX, and ABM elsewhere. Passing a word from the wrong leaf is exactly the
// silent-wrong-answer bug this table makes visible.
enum {
	// leaf 1, ECX
	X64_CPUID1_ECX_FMA     = 1u << 12,
	X64_CPUID1_ECX_SSE4_2  = 1u << 20,
	X64_CPUID1_ECX_POPCNT  = 1u << 23,
	X64_CPUID1_ECX_OSXSAVE = 1u << 27,
	X64_CPUID1_ECX_AVX     = 1u << 28,
	// leaf 7 subleaf 0, EBX
	X64_CPUID7_EBX_BMI1    = 1u << 3,
	X64_CPUID7_EBX_AVX2    = 1u << 5,
	X64_CPUID7_EBX_BMI2    = 1u << 8,
	// leaf 0x80000001, ECX
	X64_CPUIDX_ECX_LZCNT   = 1u << 5,
	// XCR0: bit 1 = SSE state, bit 2 = AVX (YMM) state. BOTH must be enabled
	// by the OS before any VEX-encoded instruction may execute.
	X64_XCR0_AVX_ENABLED   = 0x6,
};

typedef struct {
	const char *name;      // "x86-64", "x86-64-v2", ... : ST_CPU_INFO / ST_CPU

	_Bool hasPopcnt;
	_Bool hasSse42;
	_Bool hasLzcnt;
	_Bool hasBmi1;
	_Bool hasBmi2;         // SHLX/SHRX: shifts with no CL constraint, no flag clobber
	_Bool hasAvx;          // implies the OS enabled the YMM state
	_Bool hasAvx2;
	_Bool hasFma;          // one fused multiply-add for the Float intrinsic

	uint32_t leaf1Ecx;     // raw, for ST_CPU_INFO and post-mortems
	uint32_t leaf7Ebx;
	uint32_t extLeafEcx;
	uint64_t xcr0;
} X64Cpu;

// Read-only after targetCpuDetect(); defined in CpuX64.c at the BASELINE.
extern X64Cpu gX64Cpu;

// Pure decode of the CPUID words into `cpu`. No CPUID execution, no globals:
// callable with fabricated inputs, which is how the golden tests it.
void x64CpuDecode(X64Cpu *cpu, uint32_t leaf1Ecx, uint32_t leaf7Ebx,
	uint32_t extLeafEcx, uint64_t xcr0);

// Force a named feature set (ST_CPU=<name>). Returns 0 if the name is unknown.
_Bool x64CpuByName(X64Cpu *cpu, const char *name);

// The names x64CpuByName accepts, NULL-terminated (for ST_CPU=? and tests).
extern const char *const X64CpuNames[];

// The floor: plain x86-64, which is all the JIT emits today.
#define X64_CPU_BASELINE_NAME "x86-64"

#endif
