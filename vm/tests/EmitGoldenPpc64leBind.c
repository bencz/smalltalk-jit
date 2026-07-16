// On a REAL ppc64le build the arch's own golden IS its ST_ABI_EMIT_TEST, so
// bind the generic name to it (mirrors EmitGoldenPpc64Bind.c for BE). The
// big-endian golden TU is not linked here (the backends are separate by
// design), so its entry point stubs out.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "EmitGoldenPpc64leBind.c is for LITTLE-ENDIAN ppc64le builds - check vmArchTestSources in CMakeLists.txt"
#endif

#include "vm/tests/SelfTests.h"
#include <stdio.h>

int abiEmitGoldenSelfTest(const char *mode)
{
	return ppc64leEmitGoldenSelfTest(mode);
}

int ppc64EmitGoldenSelfTest(const char *mode)
{
	(void) mode;
	printf("the ppc64 (big-endian) golden TU is not linked into a ppc64le build\n");
	return 1;
}
