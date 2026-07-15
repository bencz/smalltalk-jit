// ppc64 backend SKELETON: every entry point the neutral runtime links against,
// stubbed with FAIL(). The tree compiles and links on big-endian ppc64,
// heap/scheduler/snapshot-format code runs — anything that would GENERATE
// machine code dies loudly here instead. See PORTING.md for the bring-up
// order (encoders first, golden-tested natively on x86).
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "vm/jit/ppc64/ is BIG-ENDIAN ppc64 only (ppc64le has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/CodeGenerator.h"
#include "core/Assert.h"

NativeCode *generateMethodCode(CompiledMethod *method)
{
	(void) method;
	FAIL(); // PORT_ME: ppc64 code generator not implemented yet
	return NULL;
}

NativeCode *generateDoesNotUnderstand(String *selector)
{
	(void) selector;
	FAIL();
	return NULL;
}

void generateLoadObject(AssemblerBuffer *buffer, RawObject *object, Register dst, _Bool tag)
{
	(void) buffer; (void) object; (void) dst; (void) tag;
	FAIL();
}

void generateLoadClass(AssemblerBuffer *buffer, Register src, Register dst)
{
	(void) buffer; (void) src; (void) dst;
	FAIL();
}

void generateStoreCheck(CodeGenerator *generator, Register object, Register value)
{
	(void) generator; (void) object; (void) value;
	FAIL();
}

void generateMethodLookup(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
}

void generateStackmap(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
}

void generateCCall(CodeGenerator *generator, intptr_t cFunction, size_t argsSize, _Bool storeIp)
{
	(void) generator; (void) cFunction; (void) argsSize; (void) storeIp;
	FAIL();
}

void generateMethodContextAllocation(CodeGenerator *generator, size_t size)
{
	(void) generator; (void) size;
	FAIL();
}

void generateBlockContextAllocation(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
}

void generatePushDummyContext(AssemblerBuffer *buffer)
{
	(void) buffer;
	FAIL();
}

NativeCode *buildNativeCode(CodeGenerator *generator)
{
	(void) generator;
	FAIL();
	return NULL;
}

NativeCode *buildNativeCodeFromAssembler(AssemblerBuffer *buffer)
{
	(void) buffer;
	FAIL();
	return NULL;
}
