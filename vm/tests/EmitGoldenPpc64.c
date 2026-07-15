// Placeholder until the ppc64 encoders exist: the golden emission test is the
// FIRST thing the real port implements (emitters are golden-testable natively
// on the x86 build host — see PORTING.md bring-up order).
#ifndef __powerpc64__
#error "EmitGoldenPpc64.c is ppc64-only - wire the arch's own golden TU in CMake"
#endif

#include "vm/tests/SelfTests.h"
#include <stdio.h>

int abiEmitGoldenSelfTest(const char *mode)
{
	(void) mode;
	printf("ppc64 golden emission tests: not implemented yet (skeleton backend)\n");
	return 1;
}
