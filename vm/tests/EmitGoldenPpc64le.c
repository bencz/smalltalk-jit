// Placeholder until the ppc64le (ELFv2) encoders exist: the golden emission
// test is the FIRST thing that port implements (see PORTING.md bring-up
// order and the ppc64 BE golden as the model). The big-endian golden TU
// (EmitGoldenPpc64.c) is not linked into ppc64le builds — the backends are
// separate by design — so both entry points stub out here.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "EmitGoldenPpc64le.c is ppc64le-only - wire the arch's own golden TU in CMake"
#endif

#include "vm/tests/SelfTests.h"
#include <stdio.h>

int abiEmitGoldenSelfTest(const char *mode)
{
	(void) mode;
	printf("ppc64le golden emission tests: not implemented yet (skeleton backend)\n");
	return 1;
}

int ppc64EmitGoldenSelfTest(const char *mode)
{
	(void) mode;
	printf("the ppc64 (big-endian) golden TU is not linked into a ppc64le build\n");
	return 1;
}
