// The x64 CPU-model axis: the pure DECODE of the CPUID words, the process-wide
// feature struct, and the detection itself. Unlike the POWER backends this is
// one file, because host and target always coincide for this backend: there is
// no foreign host that needs to link the decode without the detection.
//
// Contract and rationale: jit/TargetCpu.h. Nothing here is consulted by the
// code generator YET; this is the axis, not an optimization.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/Cpu.h"
#include "jit/TargetCpu.h"
#include <cpuid.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Pin our private bit table against the compiler's own <cpuid.h>, so a
// hand-copied constant can never drift from the real definition. Note how many
// of these are the same numeric value in different leaves: that is precisely
// why the decode below takes each leaf's word as a separate parameter.
_Static_assert(X64_CPUID1_ECX_FMA == bit_FMA, "cpuid bit copy drifted");
_Static_assert(X64_CPUID1_ECX_SSE4_2 == bit_SSE4_2, "cpuid bit copy drifted");
_Static_assert(X64_CPUID1_ECX_POPCNT == bit_POPCNT, "cpuid bit copy drifted");
_Static_assert(X64_CPUID1_ECX_OSXSAVE == bit_OSXSAVE, "cpuid bit copy drifted");
_Static_assert(X64_CPUID1_ECX_AVX == bit_AVX, "cpuid bit copy drifted");
_Static_assert(X64_CPUID7_EBX_BMI1 == bit_BMI, "cpuid bit copy drifted");
_Static_assert(X64_CPUID7_EBX_AVX2 == bit_AVX2, "cpuid bit copy drifted");
_Static_assert(X64_CPUID7_EBX_BMI2 == bit_BMI2, "cpuid bit copy drifted");
_Static_assert(X64_CPUIDX_ECX_LZCNT == bit_LZCNT, "cpuid bit copy drifted");

// The BASELINE: plain x86-64, which is all the JIT emits today, so a failed
// detection can only lose optimizations.
X64Cpu gX64Cpu = {
	.name = X64_CPU_BASELINE_NAME,
};

void x64CpuDecode(X64Cpu *cpu, uint32_t leaf1Ecx, uint32_t leaf7Ebx,
	uint32_t extLeafEcx, uint64_t xcr0)
{
	memset(cpu, 0, sizeof(*cpu));
	cpu->leaf1Ecx = leaf1Ecx;
	cpu->leaf7Ebx = leaf7Ebx;
	cpu->extLeafEcx = extLeafEcx;
	cpu->xcr0 = xcr0;

	// GPR-only features: what CPUID says is what you get.
	cpu->hasPopcnt = (leaf1Ecx & X64_CPUID1_ECX_POPCNT) != 0;
	cpu->hasSse42 = (leaf1Ecx & X64_CPUID1_ECX_SSE4_2) != 0;
	cpu->hasLzcnt = (extLeafEcx & X64_CPUIDX_ECX_LZCNT) != 0;
	cpu->hasBmi1 = (leaf7Ebx & X64_CPUID7_EBX_BMI1) != 0;
	cpu->hasBmi2 = (leaf7Ebx & X64_CPUID7_EBX_BMI2) != 0;

	// VEX-encoded features additionally need the OS to have enabled the YMM
	// register state, or they fault despite CPUID advertising them. OSXSAVE
	// says XGETBV is even legal to execute; XCR0 says what the OS turned on.
	_Bool osAvx = (leaf1Ecx & X64_CPUID1_ECX_OSXSAVE) != 0
		&& (xcr0 & X64_XCR0_AVX_ENABLED) == X64_XCR0_AVX_ENABLED;
	cpu->hasAvx = osAvx && (leaf1Ecx & X64_CPUID1_ECX_AVX) != 0;
	cpu->hasAvx2 = cpu->hasAvx && (leaf7Ebx & X64_CPUID7_EBX_AVX2) != 0;
	cpu->hasFma = cpu->hasAvx && (leaf1Ecx & X64_CPUID1_ECX_FMA) != 0;

	// Best-effort name along the psABI microarchitecture levels, for humans
	// only: never branch the codegen on it.
	cpu->name = (cpu->hasAvx2 && cpu->hasBmi2 && cpu->hasFma) ? "x86-64-v3"
		: (cpu->hasSse42 && cpu->hasPopcnt) ? "x86-64-v2"
		: X64_CPU_BASELINE_NAME;
}

// ST_CPU=<name>: synthesize the CPUID words a chip of that level reports and
// run them through the SAME decode, so a forced set can never disagree with a
// detected one.
static const struct {
	const char *name;
	uint32_t leaf1Ecx;
	uint32_t leaf7Ebx;
	uint32_t extLeafEcx;
	uint64_t xcr0;
} X64CpuModels[] = {
	{ X64_CPU_BASELINE_NAME, 0, 0, 0, 0 },
	{ "x86-64-v2", X64_CPUID1_ECX_SSE4_2 | X64_CPUID1_ECX_POPCNT, 0,
	  X64_CPUIDX_ECX_LZCNT, 0 },
	{ "x86-64-v3", X64_CPUID1_ECX_SSE4_2 | X64_CPUID1_ECX_POPCNT
	  | X64_CPUID1_ECX_OSXSAVE | X64_CPUID1_ECX_AVX | X64_CPUID1_ECX_FMA,
	  X64_CPUID7_EBX_BMI1 | X64_CPUID7_EBX_AVX2 | X64_CPUID7_EBX_BMI2,
	  X64_CPUIDX_ECX_LZCNT, X64_XCR0_AVX_ENABLED },
	{ NULL, 0, 0, 0, 0 },
};

const char *const X64CpuNames[] = {
	X64_CPU_BASELINE_NAME, "x86-64-v2", "x86-64-v3", NULL,
};

_Bool x64CpuByName(X64Cpu *cpu, const char *name)
{
	for (size_t i = 0; X64CpuModels[i].name != NULL; i++) {
		if (strcmp(name, X64CpuModels[i].name) == 0) {
			x64CpuDecode(cpu, X64CpuModels[i].leaf1Ecx, X64CpuModels[i].leaf7Ebx,
				X64CpuModels[i].extLeafEcx, X64CpuModels[i].xcr0);
			return 1;
		}
	}
	return 0;
}

// XGETBV(0). Only legal once CPUID reported OSXSAVE, hence the guard at the
// call site: executing it unconditionally faults on pre-2008 hardware.
static uint64_t readXcr0(void)
{
	uint32_t lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((uint64_t) hi << 32) | lo;
}

static void printCpu(const X64Cpu *cpu, const char *how)
{
	printf("x64 CPU (%s): %s [leaf1.ecx=0x%08X leaf7.ebx=0x%08X ext.ecx=0x%08X xcr0=0x%llX]"
		" popcnt=%d sse4.2=%d lzcnt=%d bmi1=%d bmi2=%d avx=%d avx2=%d fma=%d\n",
		how, cpu->name, cpu->leaf1Ecx, cpu->leaf7Ebx, cpu->extLeafEcx,
		(unsigned long long) cpu->xcr0, cpu->hasPopcnt, cpu->hasSse42, cpu->hasLzcnt,
		cpu->hasBmi1, cpu->hasBmi2, cpu->hasAvx, cpu->hasAvx2, cpu->hasFma);
}

void targetCpuDetect(void)
{
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
	uint32_t leaf1Ecx = 0, leaf7Ebx = 0, extLeafEcx = 0;
	uint64_t xcr0 = 0;
	const char *forced = getenv("ST_CPU");

	if (forced != NULL) {
		if (!x64CpuByName(&gX64Cpu, forced)) {
			printf("ST_CPU: unknown x64 CPU '%s'. Known:", forced);
			for (const char *const *n = X64CpuNames; *n != NULL; n++) {
				printf(" %s", *n);
			}
			printf("\n");
			exit(EXIT_FAILURE);
		}
		printCpu(&gX64Cpu, "forced by ST_CPU");
		return;
	}

	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
		leaf1Ecx = ecx;
	}
	if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
		leaf7Ebx = ebx;
	}
	if (__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx)) {
		extLeafEcx = ecx;
	}
	if (leaf1Ecx & X64_CPUID1_ECX_OSXSAVE) {
		xcr0 = readXcr0();
	}

	x64CpuDecode(&gX64Cpu, leaf1Ecx, leaf7Ebx, extLeafEcx, xcr0);
	if (getenv("ST_CPU_INFO") != NULL) {
		printCpu(&gX64Cpu, "detected");
	}
}
