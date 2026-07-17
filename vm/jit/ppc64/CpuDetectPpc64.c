// The ppc64 CPU-model DETECTION: the one arch-only piece of the axis, a thin
// shim that asks the OS layer for this platform's feature words and hands them
// to the pure decode in CpuPpc64.c. Everything interesting (the bit decode) is
// host-independent and golden-tested; this file is deliberately boring.
//
// Note what is NOT here: getauxval(). The ELF auxiliary vector is a LINUX
// facility, not a PowerPC one (AIX has no auxv and answers through
// _system_configuration.implementation instead; PASE follows AIX; FreeBSD
// spells it elf_aux_info). Acquiring the words is therefore an OS fact and
// lives behind osCpuFeatureWords() in vm/os/<os>/OsCpu.c, which also owns the
// static_asserts that pin our private bit table against that platform's real
// <bits/hwcap.h>. This file only knows what the BITS mean, which is
// architectural. Contrast x64, where CPUID is an unprivileged instruction and
// needs no OS at all.
//
// Contract: jit/TargetCpu.h. Called ONCE, first thing in main().
#ifndef __powerpc64__
#error "vm/jit/ppc64/ is powerpc64-only code (both byte orders) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/ppc64/Cpu.h"
#include "jit/TargetCpu.h"
#include "os/Os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void printCpu(const Ppc64Cpu *cpu, const char *how)
{
	// gprvsr is the DERIVED capability the emitter actually branches on
	// (generateBitsToFpr/generateFprToBits): print it, or the one bit that
	// decides the emission is invisible in the post-mortem.
	printf("ppc64 CPU (%s): %s [hwcap=0x%llX hwcap2=0x%llX]"
		" power6=%d power7=%d power8=%d power9=%d power10=%d altivec=%d vsx=%d gprvsr=%d\n",
		how, cpu->name, (unsigned long long) cpu->hwcap, (unsigned long long) cpu->hwcap2,
		cpu->isPower6, cpu->isPower7, cpu->isPower8, cpu->isPower9, cpu->isPower10,
		cpu->hasAltivec, cpu->hasVsx, cpu->hasGprVsrMoves);
}

void targetCpuDetect(void)
{
	uint64_t word0 = 0, word1 = 0;
	const char *forced = getenv("ST_CPU");

	if (forced != NULL) {
		// Validate against THIS target's accepted subset (cpu/CpuBind*.c):
		// the decoder knows every profile, but forcing a pre-POWER8 model on
		// ppc64le would emit for a machine that cannot exist.
		_Bool accepted = 0;
		for (const char *const *n = Ppc64CpuAccepted; *n != NULL; n++) {
			accepted |= strcmp(forced, *n) == 0;
		}
		if (!accepted || !ppc64CpuByName(&gPpc64Cpu, forced)) {
			printf("ST_CPU: unknown ppc64 CPU '%s'. Known:", forced);
			for (const char *const *n = Ppc64CpuAccepted; *n != NULL; n++) {
				printf(" %s", *n);
			}
			printf("\n");
			exit(EXIT_FAILURE);
		}
		printCpu(&gPpc64Cpu, "forced by ST_CPU");
		return;
	}

	// A platform with no discovery mechanism leaves gPpc64Cpu at the baseline,
	// which claims nothing: the JIT then emits base-ISA code, which is what it
	// emits today anyway.
	if (osCpuFeatureWords(&word0, &word1)) {
		ppc64CpuDecode(&gPpc64Cpu, word0, word1);
	}
	if (getenv("ST_CPU_INFO") != NULL) {
		printCpu(&gPpc64Cpu, "detected");
	}
}