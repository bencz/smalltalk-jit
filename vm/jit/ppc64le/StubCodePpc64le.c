// ppc64 backend SKELETON: stub descriptors + the call emitter, FAIL()-stubbed.
// (getStubNativeCode itself is neutral — vm/jit/StubCode.c.)
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vm/jit/ppc64le/ is LITTLE-ENDIAN ppc64 only (big-endian ppc64 has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/StubCode.h"
#include "core/Assert.h"

static void generateSmalltalkEntry(CodeGenerator *generator)
{
	(void) generator;
	FAIL(); // PORT_ME: ppc64 entry stub (ELFv1 needs the function-descriptor dance)
}

static void generateAllocate(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
}

static void generateLookup(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
}

static void generateDoesNotUnderstandStub(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
}

StubCode SmalltalkEntry = { .generator = generateSmalltalkEntry, .id = STUB_SMALLTALK_ENTRY };
StubCode AllocateStub = { .generator = generateAllocate, .id = STUB_ALLOCATE };
StubCode LookupStub = { .generator = generateLookup, .id = STUB_LOOKUP };
StubCode DoesNotUnderstandStub = { .generator = generateDoesNotUnderstandStub, .id = STUB_DNU };

void generateStubCall(CodeGenerator *generator, StubCode *stubCode)
{
	(void) generator; (void) stubCode;
	FAIL();
}
