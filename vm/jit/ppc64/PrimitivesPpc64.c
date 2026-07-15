// ppc64 backend SKELETON: the documented bring-up shortcut — every GEN
// primitive generator from the X-macro registry as a FAIL() stub; implement
// incrementally against golden emission tests once the encoders exist.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "vm/jit/ppc64/ is BIG-ENDIAN ppc64 only (ppc64le has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/TargetPrimitives.h"
#include "core/Assert.h"

#define GEN_PRIMITIVE(name, fn) \
	void fn(CodeGenerator *generator) { (void) generator; FAIL(); }
#include "jit/PrimitivesGen.def"
#undef GEN_PRIMITIVE

void generateCCallPrimitive(CodeGenerator *generator, PrimitiveResult (*cFunction)(), size_t argsSize)
{
	(void) generator; (void) cFunction; (void) argsSize;
	FAIL();
}
