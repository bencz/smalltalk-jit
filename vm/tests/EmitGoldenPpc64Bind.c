// On a ppc64 (big-endian) build the arch's own golden IS the ppc64 golden:
// ST_ABI_EMIT_TEST and ST_PPC64_EMIT_TEST run the same cases. (On the x86
// dev host the x64 golden owns abiEmitGoldenSelfTest and the ppc64 cases run
// only under their own env var.)
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "EmitGoldenPpc64Bind.c is for BIG-ENDIAN ppc64 builds - check vmArchTestSources in CMakeLists.txt"
#endif

#include "vm/tests/SelfTests.h"

int abiEmitGoldenSelfTest(const char *mode)
{
	return ppc64EmitGoldenSelfTest(mode);
}
