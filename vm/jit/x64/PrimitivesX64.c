// x86-64 backend. Compiled only when ST_ARCH=x64 (CMakeLists.txt); the guard
// catches a forced ST_ARCH on the wrong host.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/AssemblerX64.h"
#include "jit/x64/Abi.h"
#include "jit/TargetPrimitives.h"
#include "core/Exception.h"
#include "core/StackFrame.h"
#include "core/CompiledCode.h"
#include "jit/StubCode.h"
#include "jit/CodeDescriptors.h"

static void generateBlockValuePrimitive(CodeGenerator *generator, uint8_t args);
static void movArg(AssemblerBuffer *buffer, ptrdiff_t index, Register dst);
static MemoryOperand arg(ptrdiff_t index);


void generateCCallPrimitive(CodeGenerator *generator, PrimitiveResult (*cFunction)(), size_t argsSize)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel failed;
	asmInitLabel(&failed);

	// Arg marshalling and PrimitiveResult decoding are C-ABI operations (SysV:
	// register args + RAX:RDX struct return; Win64: sret pointer) — the hooks
	// live in the selected abi/<abi>/ instance. The R11<->R13 native-code dance,
	// the ret and the fail fall-through are VM protocol and stay shared.
	gX64Abi->emitCCallPrimArgs(buffer, argsSize);
	asmMovq(buffer, R11, R13);
	generateCCall(generator, (intptr_t) cFunction, argsSize, 0);
	gX64Abi->emitPrimResultCheck(buffer, &failed);
	asmRet(buffer);
	asmLabelBind(buffer, &failed, asmOffset(buffer));
	asmMovq(buffer, R13, R11);
}


static void loadClass(CodeGenerator *generator, Register src, Register dst)
{
	asmMovqMem(&generator->buffer, asmMem(src, NO_REGISTER, SS_1, -1), dst);
}


static void testInt(CodeGenerator *generator, ByteRegister reg)
{
	asmTestbImm(&generator->buffer, reg, 3);
}


static void primitveNotImplemented(void)
{
	printf("Error: Primitive is not implemented\n");
	exit(EXIT_FAILURE);
}


void generateNotImplementedPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	asmMovqImm(buffer, (uint64_t) primitveNotImplemented, TMP);
	asmCallq(buffer, TMP);
}


void generateInstVarAtPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel outOfBounds;
	ptrdiff_t offset = offsetof(RawClass, instanceShape);
	ptrdiff_t sizeOffset = offset + offsetof(InstanceShape, varsSize);
	ptrdiff_t isIndexedOffset = offset + offsetof(InstanceShape, isIndexed);
	ptrdiff_t payloadSizeOffset = offset + offsetof(InstanceShape, payloadSize);

	asmInitLabel(&notInt);
	asmInitLabel(&outOfBounds);

	movArg(buffer, 0, RDI);
	movArg(buffer, 1, RSI);

	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	loadClass(generator, RDI, RCX);
	asmShrqImm(buffer, RSI, 2);
	asmDecq(buffer, RSI);
	asmCmpbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, sizeOffset), SIL);
	asmJ(buffer, COND_ABOVE_EQUAL, &outOfBounds);

	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, isIndexedOffset), SIL);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, payloadSizeOffset), SIL);
	asmMovqMem(buffer, asmMem(RDI, RSI, SS_8, HEADER_SIZE - 1), RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmIncq(buffer, RSI);
	asmShlqImm(buffer, RSI, 2);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateInstVarAtPutPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel outOfBounds;
	ptrdiff_t offset = offsetof(RawClass, instanceShape);
	ptrdiff_t sizeOffset = offset + offsetof(InstanceShape, varsSize);
	ptrdiff_t isIndexedOffset = offset + offsetof(InstanceShape, isIndexed);
	ptrdiff_t payloadSizeOffset = offset + offsetof(InstanceShape, payloadSize);

	asmInitLabel(&notInt);
	asmInitLabel(&outOfBounds);

	movArg(buffer, 0, RDI);
	movArg(buffer, 1, RSI);
	movArg(buffer, 2, RAX);

	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	loadClass(generator, RDI, RCX);
	asmShrqImm(buffer, RSI, 2);
	asmDecq(buffer, RSI);
	asmCmpbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, sizeOffset), SIL);
	asmJ(buffer, COND_ABOVE_EQUAL, &outOfBounds);

	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, isIndexedOffset), SIL);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, payloadSizeOffset), SIL);
	asmMovqToMem(buffer, RAX, asmMem(RDI, RSI, SS_8, HEADER_SIZE - 1));
	generateStoreCheck(generator, RDI, RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmIncq(buffer, RSI);
	asmShlqImm(buffer, RSI, 2);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateAtPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel notIndexable;
	AssemblerLabel outOfBounds;
	AssemblerLabel bytes;
	ptrdiff_t offset = offsetof(RawClass, instanceShape);
	ptrdiff_t payloadSizeOffset = offset + offsetof(InstanceShape, payloadSize);
	ptrdiff_t sizeOffset = offset + offsetof(InstanceShape, varsSize);
	ptrdiff_t isIndexedOffset = offset + offsetof(InstanceShape, isIndexed);
	ptrdiff_t isBytesOffset = offset + offsetof(InstanceShape, isBytes);
	ptrdiff_t valueTypeOffset = offset + offsetof(InstanceShape, valueType);

	asmInitLabel(&notInt);
	asmInitLabel(&notIndexable);
	asmInitLabel(&outOfBounds);
	asmInitLabel(&bytes);

	movArg(buffer, 0, RDI);
	movArg(buffer, 1, RSI);

	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	loadClass(generator, RDI, RCX);
	asmTestbMemImm(buffer, asmMem(RCX, NO_REGISTER, SS_1, isIndexedOffset), 1);
	asmJ(buffer, COND_ZERO, &notIndexable);

	asmShrqImm(buffer, RSI, 2);
	asmDecq(buffer, RSI);
	asmCmpqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, offsetof(RawIndexedObject, size) - 1), RSI);
	asmJ(buffer, COND_ABOVE_EQUAL, &outOfBounds);

	asmTestbMemImm(buffer, asmMem(RCX, NO_REGISTER, SS_1, isBytesOffset), 1);
	asmJ(buffer, COND_NOT_ZERO, &bytes);

	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, payloadSizeOffset), SIL);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, sizeOffset), SIL);
	asmMovqMem(buffer, asmMem(RDI, RSI, SS_8, HEADER_SIZE + sizeof(Value) - 1), RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &bytes, asmOffset(buffer));
	asmXorq(buffer, RAX, RAX);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, payloadSizeOffset), AL);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, sizeOffset), AL);
	asmLeaq(buffer, asmMem(RSI, RAX, SS_8, sizeof(Value)), RAX);
	asmMovzxbMemq(buffer, asmMem(RDI, RAX, SS_1, HEADER_SIZE - 1), RAX);
	asmShlqImm(buffer, RAX, 2);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, valueTypeOffset), AL);
	asmRet(buffer);

	asmLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmIncq(buffer, RSI);
	asmShlqImm(buffer, RSI, 2);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &notIndexable, asmOffset(buffer));
}


void generateAtPutPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel notIndexable;
	AssemblerLabel outOfBounds;
	AssemblerLabel bytes;
	ptrdiff_t offset = offsetof(RawClass, instanceShape);
	ptrdiff_t payloadSizeOffset = offset + offsetof(InstanceShape, payloadSize);
	ptrdiff_t sizeOffset = offset + offsetof(InstanceShape, varsSize);
	ptrdiff_t isIndexedOffset = offset + offsetof(InstanceShape, isIndexed);
	ptrdiff_t isBytesOffset = offset + offsetof(InstanceShape, isBytes);
	ptrdiff_t valueTypeOffset = offset + offsetof(InstanceShape, valueType);

	asmInitLabel(&notInt);
	asmInitLabel(&notIndexable);
	asmInitLabel(&outOfBounds);
	asmInitLabel(&bytes);

	// load arguments
	movArg(buffer, 0, RDI);
	movArg(buffer, 1, RSI);
	movArg(buffer, 2, RDX);

	// value is result
	asmMovq(buffer, RDX, RAX);

	// check if index is int
	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	// check if object is indexed
	loadClass(generator, RDI, RCX);
	asmTestbMemImm(buffer, asmMem(RCX, NO_REGISTER, SS_1, isIndexedOffset), 1);
	asmJ(buffer, COND_ZERO, &notIndexable);

	// untag and decrement index
	asmShrqImm(buffer, RSI, 2);
	asmDecq(buffer, RSI);

	// check bounds
	asmCmpqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, offsetof(RawIndexedObject, size) - 1), RSI);
	asmJ(buffer, COND_ABOVE_EQUAL, &outOfBounds);

	// check if object is bytes
	asmTestbMemImm(buffer, asmMem(RCX, NO_REGISTER, SS_1, isBytesOffset), 1);
	asmJ(buffer, COND_NOT_ZERO, &bytes);

	// compute offset
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, payloadSizeOffset), SIL);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, sizeOffset), SIL);

	// set the value
	asmMovqToMem(buffer, RDX, asmMem(RDI, RSI, SS_8, HEADER_SIZE + sizeof(Value) - 1));
	generateStoreCheck(generator, RDI, RDX);
	asmRet(buffer);

	// bytes
	asmLabelBind(buffer, &bytes, asmOffset(buffer));

	// compute offset
	asmXorq(buffer, RBX, RBX);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, payloadSizeOffset), AL);
	asmAddbMem(buffer, asmMem(RCX, NO_REGISTER, SS_1, sizeOffset), AL);
	asmLeaq(buffer, asmMem(RSI, RBX, SS_8, sizeof(Value)), RBX);

	// TODO: valueTypeOf(RDX) == RCX->instanceShape.valueType
	// untag and set the value
	asmShrqImm(buffer, RDX, 2);
	asmMovbToMem(buffer, DL, asmMem(RDI, RBX, SS_1, HEADER_SIZE - 1));
	asmRet(buffer);

	asmLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmIncq(buffer, RSI);
	asmShlqImm(buffer, RSI, 2);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &notIndexable, asmOffset(buffer));
}


void generateSizePrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notPointer;
	AssemblerLabel notIndexable;
	ptrdiff_t offset = offsetof(RawClass, instanceShape);
	ptrdiff_t isIndexedOffset = offset + offsetof(InstanceShape, isIndexed);

	asmInitLabel(&notPointer);
	asmInitLabel(&notIndexable);

	movArg(buffer, 0, RDI);

	asmMovb(buffer, DIL, SIL);
	asmAndbImm(buffer, SIL, 3);
	asmCmpbImm(buffer, SIL, VALUE_POINTER);
	asmJ(buffer, COND_NOT_EQUAL, &notPointer);

	loadClass(generator, RDI, RDX);
	asmTestbMemImm(buffer, asmMem(RDX, NO_REGISTER, SS_1, isIndexedOffset), 1);
	asmJ(buffer, COND_ZERO, &notIndexable);

	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, offsetof(RawIndexedObject, size) - 1), RAX);
	asmShlqImm(buffer, RAX, 2);
	asmRet(buffer);

	asmLabelBind(buffer, &notPointer, asmOffset(buffer));
	asmLabelBind(buffer, &notIndexable, asmOffset(buffer));
	asmXorq(buffer, RAX, RAX);
	asmRet(buffer);
}


void generateIdentityPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel equal;
	asmInitLabel(&equal);

	movArg(buffer, 0, RDI);
	asmCmpqMem(buffer, arg(1), RDI);
	asmJ(buffer, COND_EQUAL, &equal);
	generateLoadObject(buffer, Handles.false->raw, RAX, 1);
	asmRet(buffer);

	asmLabelBind(buffer, &equal, asmOffset(buffer));
	generateLoadObject(buffer, Handles.true->raw, RAX, 1);
	asmRet(buffer);
}


void generateHashPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, RDI);
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, /*offsetof(Object, hash)*/ sizeof(Value) - 1), RAX);
	asmMovqImm(buffer, 0xFFFFFFFF, TMP);
	asmAndq(buffer, TMP, RAX);
	asmShlqImm(buffer, RAX, 2);
	asmRet(buffer);
}


void generateClassPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, RDI);
	generateLoadClass(buffer, RDI, RAX);
	asmRet(buffer);
}


void generateBehaviorNewPrimitive(CodeGenerator *generator)
{
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);
	AssemblerBuffer *buffer = &generator->buffer;

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RSI);
	asmDecq(buffer, RSI);
	asmXorq(buffer, RDX, RDX);
	generateStubCall(generator, &AllocateStub);

	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
	asmRet(buffer);
}


void generateBehaviorNewSizePrimitive(CodeGenerator *generator)
{
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)), RDX);

	// check if size is int
	testInt(generator, DL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RSI);
	asmDecq(buffer, RSI);
	asmShrqImm(buffer, RDX, 2);
	generateStubCall(generator, &AllocateStub);

	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
}


void generateCharacterNewPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, RAX);
	testInt(generator, AL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	asmAddqImm(buffer, RAX, VALUE_CHAR);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateCharacterCodePrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, RAX);
	asmSubqImm(buffer, RAX, VALUE_CHAR);
	asmRet(buffer);
}


void generateStringHashPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, RDI);
	asmDecq(buffer, RDI);
	asmMovqImm(buffer, (uint64_t) computeRawStringHash, TMP);
	asmCallq(buffer, TMP);
	asmShlqImm(buffer, RAX, 2);
	asmRet(buffer);
}


void generateInterruptPrimitive(CodeGenerator *generator)
{
	asmInt3(&generator->buffer);
}


void generateExitPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	asmMovqImm(buffer, 1, RDI);
	asmMovqImm(buffer, (uint64_t) exit, TMP);
	asmCallq(buffer, TMP);
}


static NativeCodeEntry scopedGetNativeCode(Value vReceiver, Value vMethod)
{
    HandleScope scope;
    openHandleScope(&scope);

    Class *class = scopeHandle(getClassOf(vReceiver));
    CompiledMethod *method = scopeHandle(asObject(vMethod));

    union PointerConverter converter;
    converter.object_pointer = getNativeCode(class, method)->insts;
    NativeCodeEntry entry = converter.function_pointer;

    closeHandleScope(&scope, NULL);
    return entry;
}


void generateMethodSendPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;

	// prologue
	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	// get native code for given method
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RSI);
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)), RDI);
	generateCCall(generator, (intptr_t) scopedGetNativeCode, 2, 1);
	// push receiver
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)));
	// invoke method
	asmMovq(buffer, RAX, R11);
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// epilogue
	asmAddqImm(buffer, RSP, 3 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);
}


void generateMethodSendArgsPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel invalidArgs;
	AssemblerLabel loop;
	AssemblerLabel zeroArgs;
	ptrdiff_t argsOffset = offsetof(RawCompiledMethod, header) + offsetof(CompiledCodeHeader, argsSize) - 1;

	asmInitLabel(&invalidArgs);
	asmInitLabel(&loop);
	asmInitLabel(&zeroArgs);

	// prologue
	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	// get native code for given method
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RSI);
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)), RDI);
	generateCCall(generator, (intptr_t) scopedGetNativeCode, 2, 1);

	// load arguments array
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 4 * sizeof(intptr_t)), RDI);
	// load arguments array size
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawArray, size)), RBX);
	// load compiled method
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), TMP);
	// check arguments size
	asmCmpbMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, argsOffset), BL);
	asmJ(buffer, COND_NOT_EQUAL, &invalidArgs);

	// push arguments on stack
	asmTestq(buffer, RBX, RBX);
	asmJ(buffer, COND_ZERO, &zeroArgs);
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	asmDecq(buffer, RBX);
	asmPushqMem(buffer, asmMem(RDI, RBX, SS_8, varOffset(RawArray, vars)));
	asmJ(buffer, COND_NOT_ZERO, &loop);
	asmLabelBind(buffer, &zeroArgs, asmOffset(buffer));

	// push receiver
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)));

	// invoke method
	asmMovq(buffer, RAX, R11);
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// epilogue
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
	asmRet(buffer);

	asmLabelBind(buffer, &invalidArgs, asmOffset(buffer));
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
}


/*static void generateSmallIntCheck(buffer)
{
	asmMovq(buffer, RDI, TMP);
	asmOr(buffer, RSI, TMP);
	asmTestqRegImm(buffer, TMP, VALUE_INT);
	asmJ(buffer, COND_NO_ZERO, )
}*/


void generateIntLessThanPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel less;
	AssemblerLabel notInt;
	asmInitLabel(&less);
	asmInitLabel(&notInt);

	movArg(buffer, 1, RSI);
	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	movArg(buffer, 0, RAX);
	asmCmpq(buffer, RAX, RSI);

	asmJ(buffer, COND_LESS, &less);
	generateLoadObject(buffer, Handles.false->raw, RAX, 1);
	asmRet(buffer);

	asmLabelBind(buffer, &less, asmOffset(buffer));
	generateLoadObject(buffer, Handles.true->raw, RAX, 1);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntAddPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel overflow;
	asmInitLabel(&notInt);
	asmInitLabel(&overflow);

	movArg(buffer, 1, RSI);
	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	movArg(buffer, 0, RAX);
	asmAddq(buffer, RSI, RAX);
	asmJ(buffer, COND_OVERFLOW, &overflow);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &overflow, asmOffset(buffer));
}


void generateIntSubPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel overflow;
	asmInitLabel(&notInt);
	asmInitLabel(&overflow);

	movArg(buffer, 1, RSI);
	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	movArg(buffer, 0, RAX);
	asmSubq(buffer, RSI, RAX);
	asmJ(buffer, COND_OVERFLOW, &overflow);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &overflow, asmOffset(buffer));
}


void generateIntMulPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel overflow;
	asmInitLabel(&notInt);
	asmInitLabel(&overflow);

	movArg(buffer, 1, RAX);
	testInt(generator, AL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	asmShrqImm(buffer, RAX, 2);
	asmImulqMem(buffer, arg(0), RAX);
	asmJ(buffer, COND_OVERFLOW, &overflow);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &overflow, asmOffset(buffer));
}


void generateIntQuoPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel divZero;
	asmInitLabel(&notInt);
	asmInitLabel(&divZero);

	movArg(buffer, 1, RSI);
	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);
	asmTestq(buffer, RSI, RSI);                 // divide-by-zero would SIGFPE: fail the
	asmJ(buffer, COND_ZERO, &divZero);          // primitive so the Smalltalk fallback raises

	movArg(buffer, 0, RAX);
	asmCqo(buffer);
	asmIdivq(buffer, RSI);
	asmShlqImm(buffer, RAX, 2);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &divZero, asmOffset(buffer));
}


void generateIntModPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel negativeResult;
	AssemblerLabel negativeDivisor;
	AssemblerLabel divZero;
	asmInitLabel(&notInt);
	asmInitLabel(&negativeResult);
	asmInitLabel(&negativeDivisor);
	asmInitLabel(&divZero);

	movArg(buffer, 1, RDI);
	testInt(generator, DIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);
	asmTestq(buffer, RDI, RDI);                 // divide-by-zero would SIGFPE: fail the
	asmJ(buffer, COND_ZERO, &divZero);          // primitive so the Smalltalk fallback raises

	movArg(buffer, 0, RAX);
	asmCqo(buffer);
	asmIdivq(buffer, RDI);
	asmMovq(buffer, RDX, RAX);

	asmXorq(buffer, RDI, RDX);
	asmCmpqImm(buffer, RDX, 0);
	asmJ(buffer, COND_LESS, &negativeResult);
	asmRet(buffer);

	// negative
	asmLabelBind(buffer, &negativeResult, asmOffset(buffer));
	asmLabelBind(buffer, &negativeDivisor, asmOffset(buffer));
	asmAddq(buffer, RDI, RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &divZero, asmOffset(buffer));
}


void generateIntRemPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel divZero;
	asmInitLabel(&notInt);
	asmInitLabel(&divZero);

	movArg(buffer, 1, RDI);
	testInt(generator, DIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);
	asmTestq(buffer, RDI, RDI);                 // divide-by-zero would SIGFPE: fail the
	asmJ(buffer, COND_ZERO, &divZero);          // primitive so the Smalltalk fallback raises

	movArg(buffer, 0, RAX);
	asmCqo(buffer);
	asmIdivq(buffer, RDI);
	asmMovq(buffer, RDX, RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
	asmLabelBind(buffer, &divZero, asmOffset(buffer));
}


void generateIntNegPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel overflow;
	asmInitLabel(&overflow);

	movArg(buffer, 0, RAX);
	asmNegq(buffer, RAX);
	asmJ(buffer, COND_OVERFLOW, &overflow);
	asmRet(buffer);

	asmLabelBind(buffer, &overflow, asmOffset(buffer));
}


void generateIntAndPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, RAX);
	testInt(generator, AL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	asmAndqMem(buffer, arg(0), RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntOrPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, RAX);
	testInt(generator, AL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	asmOrqMem(buffer, arg(0), RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntXorPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, RAX);
	testInt(generator, AL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	asmXorqMem(buffer, arg(0), RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntShiftPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel rightShift;
	asmInitLabel(&notInt);
	asmInitLabel(&rightShift);

	movArg(buffer, 1, RSI);
	testInt(generator, SIL);
	asmJ(buffer, COND_NOT_ZERO, &notInt);

	movArg(buffer, 0, RAX);
	asmSarqImm(buffer, RSI, 2);
	asmMovb(buffer, SIL, CL);
	asmTestq(buffer, RSI, RSI);
	asmJ(buffer, COND_SIGN, &rightShift);
	asmShlq(buffer, RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &rightShift, asmOffset(buffer));
	asmNegb(buffer, CL);
	asmShrq(buffer, RAX);
	asmAndqImm(buffer, RAX, ~3);
	asmRet(buffer);

	asmLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateBlockValuePrimitive0Args(CodeGenerator *generator)
{
	generateBlockValuePrimitive(generator, 0);
}


void generateBlockValuePrimitive1Args(CodeGenerator *generator)
{
	generateBlockValuePrimitive(generator, 1);
}


void generateBlockValuePrimitive2Args(CodeGenerator *generator)
{
	generateBlockValuePrimitive(generator, 2);
}


void generateBlockValuePrimitive3Args(CodeGenerator *generator)
{
	generateBlockValuePrimitive(generator, 3);
}


static void generateBlockValuePrimitive(CodeGenerator *generator, uint8_t args)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel invalidArgs;
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);
	ptrdiff_t argsOffset = offsetof(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, argsSize) - 1;
	uint8_t i;

	asmInitLabel(&invalidArgs);

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	// push current context
	asmPushq(buffer, CTX);

	// load compiled block
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), TMP);
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawBlock, compiledBlock)), TMP);

	// check arguments size
	asmCmpbMemImm(buffer, asmMem(TMP, NO_REGISTER, SS_1, argsOffset), args);
	asmJ(buffer, COND_NOT_EQUAL, &invalidArgs);

	generateBlockContextAllocation(generator);

	// epilogue
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);

	// replace receiver on stack
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawBlock, receiver)), TMP);
	asmMovqToMem(buffer, TMP, arg(0));

	// call block
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawBlock, nativeCode)), R11);
	asmAddqImm(buffer, R11, offsetof(NativeCode, insts));
	asmJmpq(buffer, R11);

	asmLabelBind(buffer, &invalidArgs, asmOffset(buffer));

	// epilogue
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
}


void generateBlockValueArgsPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel invalidArgs;
	AssemblerLabel loop;
	AssemblerLabel zeroArgs;
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);
	ptrdiff_t argsOffset = offsetof(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, argsSize) - 1;
	uint8_t i;

	asmInitLabel(&invalidArgs);
	asmInitLabel(&loop);
	asmInitLabel(&zeroArgs);

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	// push current context
	asmPushq(buffer, CTX);

	// load block and arguments array
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), R13);
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)), RSI);

	// load compiled block
	asmMovqMem(buffer, asmMem(R13, NO_REGISTER, SS_1, varOffset(RawBlock, compiledBlock)), TMP);

	// load array arguments size
	asmMovqMem(buffer, asmMem(RSI, NO_REGISTER, SS_1, varOffset(RawArray, size)), RBX);

	// check arguments size
	asmCmpbMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, argsOffset), BL);
	asmJ(buffer, COND_NOT_EQUAL, &invalidArgs);

	// push arguments on stack
	asmTestq(buffer, RBX, RBX);
	asmJ(buffer, COND_ZERO, &zeroArgs);
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	asmDecq(buffer, RBX);
	asmPushqMem(buffer, asmMem(RSI, RBX, SS_8, varOffset(RawArray, vars)));
	asmJ(buffer, COND_NOT_ZERO, &loop);
	asmLabelBind(buffer, &zeroArgs, asmOffset(buffer));

	// push receiver
	asmPushqMem(buffer, asmMem(R13, NO_REGISTER, SS_1, varOffset(RawBlock, receiver)));

	generateBlockContextAllocation(generator);

	// call block
	asmMovqMem(buffer, asmMem(R13, NO_REGISTER, SS_1, varOffset(RawBlock, nativeCode)), RAX);
	asmAddqImm(buffer, RAX, offsetof(NativeCode, insts));
	asmCallq(buffer, RAX);

	// epilogue
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
	asmRet(buffer);

	asmLabelBind(buffer, &invalidArgs, asmOffset(buffer));
	asmMovq(buffer, RBP, RSP);
	asmPopq(buffer, RBP);
}


void generateBlockWhileTrue(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel loop;
	AssemblerLabel notBoolean;
	asmInitLabel(&loop);
	asmInitLabel(&notBoolean);

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, RDI, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, RSI, 0);
	generateMethodLookup(generator);

	asmPushq(buffer, R11);
	asmSubqImm(buffer, RSP, sizeof(intptr_t));

	// value block
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	// copy receiver in loop again as #blockValue replaces it
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(RSP, NO_REGISTER, SS_1, 0));
	// call block native code
	asmMovqMem(buffer, asmMem(RSP, NO_REGISTER, SS_1, sizeof(intptr_t)), R11);
	// Reload CTX from this frame's stable context slot so the block parents to the
	// same context every iteration (see generateBlockWhileTrue2 for the rationale —
	// prevents an unbounded BlockContext parent-chain when the body parks/resumes).
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), CTX);
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// repeat if result is true
	generateLoadObject(buffer, Handles.true->raw, TMP, 1);
	asmCmpq(buffer, RAX, TMP);
	asmJ(buffer, COND_EQUAL, &loop);

	// check if result is false
	generateLoadObject(buffer, Handles.false->raw, TMP, 1);
	asmCmpq(buffer, RAX, TMP);
	asmJ(buffer, COND_NOT_EQUAL, &notBoolean);

	// return receiver
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RAX);
	asmAddqImm(buffer, RSP, 4 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);

	// not boolean
	asmLabelBind(buffer, &notBoolean, asmOffset(buffer));
	asmAddqImm(buffer, RSP, 3 * sizeof(intptr_t));
	asmPopq(buffer, R11);
	asmPopq(buffer, RBP);
}


void generateBlockWhileTrue2(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);
	AssemblerLabel loop;
	AssemblerLabel end;
	AssemblerLabel notBoolean;
	asmInitLabel(&loop);
	asmInitLabel(&end);
	asmInitLabel(&notBoolean);

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, RDI, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, RSI, 0);
	generateMethodLookup(generator);
	asmPushq(buffer, R11);

	// value block
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)));
	asmMovqMem(buffer, asmMem(RSP, NO_REGISTER, SS_1, sizeof(intptr_t)), R11);
	// Reload CTX from this frame's stable context slot ([RBP-2*8], the pushed
	// dummy) so the invoked block's context always parents to the SAME context
	// every iteration. Without this, the block-value primitive parents each
	// iteration's context to whatever CTX happens to hold — which, when the block
	// parks and resumes across a fiber switch inside a nested call, drifts into the
	// previous iteration's context and builds an unbounded parent chain that a full
	// GC keeps alive (a per-iteration BlockContext leak in every blocking loop).
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), CTX);
	asmCallq(buffer, R11);
	generateStackmap(generator);
	asmAddqImm(buffer, RSP, sizeof(intptr_t));

	// repeat if result is true
	generateLoadObject(buffer, Handles.true->raw, TMP, 1);
	asmCmpq(buffer, RAX, TMP);
	asmJ(buffer, COND_NOT_EQUAL, &end);

	// value second block
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)));
	asmMovqMem(buffer, asmMem(RSP, NO_REGISTER, SS_1, sizeof(intptr_t)), R11);
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), CTX);
	asmCallq(buffer, R11);
	generateStackmap(generator);
	asmAddqImm(buffer, RSP, sizeof(intptr_t));
	asmJmpLabel(buffer, &loop);

	// result is not true
	asmLabelBind(buffer, &end, asmOffset(buffer));

	// check if result is false
	generateLoadObject(buffer, Handles.false->raw, TMP, 1);
	asmCmpq(buffer, RAX, TMP);
	asmJ(buffer, COND_NOT_EQUAL, &notBoolean);

	// return receiver
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RAX);
	asmAddqImm(buffer, RSP, 3 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);

	// not boolean
	asmLabelBind(buffer, &notBoolean, asmOffset(buffer));
	asmAddqImm(buffer, RSP, 2 * sizeof(intptr_t));
	asmPopq(buffer, R11);
	asmPopq(buffer, RBP);
}


void generateBlockOnExceptionPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);
	AssemblerFixup *ip;
	generator->frameSize = 2;

	// prologue
	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);
	asmSubqImm(buffer, RSP, generator->frameSize * sizeof(intptr_t));

	// save native code
	asmMovqToMem(buffer, R11, asmMem(RBP, NO_REGISTER, SS_1, -sizeof(intptr_t)));
	generateMethodContextAllocation(generator, 0);

	// allocate exception handler
	generateLoadObject(buffer, (RawObject *) Handles.ExceptionHandler->raw, RSI, 0);
	asmXorq(buffer, RDX, RDX);
	generateStubCall(generator, &AllocateStub);
	// setup return IP
	asmMovqImm(buffer, 0, TMP);
	ip = asmEmitFixup(buffer, ASM_FIXUP_IP, sizeof(intptr_t), asmOffset(buffer) - sizeof(intptr_t));
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, ip)));
	// setup context exception handler
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), R9);
	asmMovqToMem(buffer, R9, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, context)));
	// spill exception handler
	asmPushq(buffer, RAX);
	generator->frameSize++;

	// install exception handler into the RUNNING worker's chain (CTX->thread->exceptionHandler),
	// NOT a baked &CurrentExceptionHandler — that immediate would be the CODEGEN thread's slot in
	// this shared JIT code, so on:do: on any other worker would corrupt the wrong chain.
	asmLoadTls(buffer, RDI, gCurrentThreadTpoff); // RDI = &CurrentThread (running worker)
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, offsetof(Thread, exceptionHandler)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, parent)));
	asmMovqToMem(buffer, RAX, asmMem(RDI, NO_REGISTER, SS_1, offsetof(Thread, exceptionHandler)));

	// value block
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, RDI, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, RSI, 0);
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)));
	generator->frameSize++;
	generateMethodLookup(generator);
	generator->frameSize--;
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// unregister exception handler (restore parent into the RUNNING worker's chain)
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -3 * sizeof(intptr_t)), TMP);
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, parent)), TMP);
	asmLoadTls(buffer, RDI, gCurrentThreadTpoff); // RDI = &CurrentThread (running worker)
	asmMovqToMem(buffer, TMP, asmMem(RDI, NO_REGISTER, SS_1, offsetof(Thread, exceptionHandler)));

	// epilogue
	asmMovqMem(&generator->buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, parent)), CTX);
	asmAddqImm(buffer, RSP, 4 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);

	// jumped from exception signal
	// RBP, RSP are restored by exception signal
	ip->value = asmOffset(buffer) - ip->offset;
	generator->frameSize = 5; // native code + context + backtrace + exception + block

	// restore context
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), CTX);

	// value exception block
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 4 * sizeof(intptr_t)));
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, RDI, 1);
	generateMethodLookup(generator);
	generator->frameSize -= 3;
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// epilogue
	asmMovqMem(&generator->buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, parent)), CTX);
	asmAddqImm(buffer, RSP, 5 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);
}


// valueUnwindProtected: aBlock (the ensure:/ifCurtailed: engine): evaluate the
// receiver block with `aBlock` registered as a pending unwind cleanup on the
// running fiber's chain. On normal completion the registration is unlinked and
// the receiver's value answered; on any unwind (exception, non-local return,
// terminate) the unwinder runs the cleanup while it cuts through this frame.
// Same skeleton as on:do: above, minus the re-entry ip.
void generateBlockUnwindPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	generator->frameSize = 2;

	// prologue
	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);
	asmSubqImm(buffer, RSP, generator->frameSize * sizeof(intptr_t));

	// save native code
	asmMovqToMem(buffer, R11, asmMem(RBP, NO_REGISTER, SS_1, -sizeof(intptr_t)));
	generateMethodContextAllocation(generator, 0);

	// allocate the unwind handler
	generateLoadObject(buffer, (RawObject *) Handles.UnwindHandler->raw, RSI, 0);
	asmXorq(buffer, RDX, RDX);
	generateStubCall(generator, &AllocateStub);
	// handler->context (freshly allocated handler: no store barrier needed)
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), R9);
	asmMovqToMem(buffer, R9, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawUnwindHandler, context)));
	// handler->block = the cleanup argument
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 3 * sizeof(intptr_t)), R9);
	asmMovqToMem(buffer, R9, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawUnwindHandler, block)));
	// spill the unwind handler
	asmPushq(buffer, RAX);
	generator->frameSize++;

	// link into the RUNNING worker's chain (TLS; same rationale as on:do:)
	asmLoadTls(buffer, RDI, gCurrentThreadTpoff);
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, offsetof(Thread, unwindHandler)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawUnwindHandler, parent)));
	asmMovqToMem(buffer, RAX, asmMem(RDI, NO_REGISTER, SS_1, offsetof(Thread, unwindHandler)));

	// value the protected block
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, RDI, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, RSI, 0);
	asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)));
	generator->frameSize++;
	generateMethodLookup(generator);
	generator->frameSize--;
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// unlink on normal completion (the unwinders unlink on the abnormal paths)
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -3 * sizeof(intptr_t)), TMP);
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawUnwindHandler, parent)), TMP);
	asmLoadTls(buffer, RDI, gCurrentThreadTpoff);
	asmMovqToMem(buffer, TMP, asmMem(RDI, NO_REGISTER, SS_1, offsetof(Thread, unwindHandler)));

	// epilogue
	asmMovqMem(&generator->buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, parent)), CTX);
	asmAddqImm(buffer, RSP, 4 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);
}


void generateExceptionSignalPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel handlerNotFound;
	AssemblerLabel skipBacktrace;
	asmInitLabel(&handlerNotFound);
	asmInitLabel(&skipBacktrace);

	// prologue
	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	asmMovq(buffer, R11, R13);

	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RDI);
	asmDecq(buffer, RDI);

	generateCCall(generator, (intptr_t) unwindExceptionHandler, 1, 1);
	asmTestq(buffer, RAX, RAX);
	asmJ(buffer, COND_ZERO, &handlerNotFound);

	// load context
	asmMovqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, context)), CTX);
	// load handler frame
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, frame)), TMP);
	// load handler block
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, 4 * sizeof(intptr_t)), TMP);
	// load compiled block
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawBlock, compiledBlock)), TMP);
	// check if block accepts backtrace in second argument
	ptrdiff_t argsSizeOffset = varOffset(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, argsSize);
	asmCmpbMemImm(buffer, asmMem(TMP, NO_REGISTER, SS_1, argsSizeOffset), 2);
	asmJ(buffer, COND_LESS, &skipBacktrace);

	asmPushq(buffer, RAX);
	generator->frameSize++;

	// generate backtrace
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), TMP);
	asmPushq(buffer, TMP);
	generator->frameSize++;
	generateLoadClass(buffer, TMP, RDI);
	generateLoadObject(buffer, (RawObject *) Handles.generateBacktraceSymbol->raw, RSI, 0);
	generateMethodLookup(generator);
	generator->frameSize--;
	asmCallq(buffer, R11);
	generateStackmap(generator);

	asmAddqImm(buffer, RSP, sizeof(intptr_t));
	asmPopq(buffer, RDI);
	generator->frameSize--;

	// load signaled exception as this stack frame is later destroyed
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), R9);
	// load context
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, context)), CTX);
	// restore SP and BP
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, frame)), RBP);
	asmLeaq(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), RSP);
	// the jump into the handler abandons everything below the handler frame,
	// including nested C entry records / handle scopes created since (a cleanup
	// that signals is the common source): drop them NOW, after the last read of
	// a dying frame. unwindThreadStateTo never allocates, so the raw pushes of
	// heap pointers around it are safe.
	asmPushq(buffer, RDI); // handler
	asmPushq(buffer, RAX); // backtrace
	asmPushq(buffer, R9);  // exception
	asmMovq(buffer, RBP, RDI);
	generateCCall(generator, (intptr_t) unwindThreadStateTo, 1, 0);
	asmPopq(buffer, R9);
	asmPopq(buffer, RAX);
	asmPopq(buffer, RDI);
	asmPushq(buffer, RAX); // push generated backtrace
	asmPushq(buffer, R9); // push signaled exception
	// load #value:value: as backtrace is not passed to the handler block
	generateLoadObject(buffer, (RawObject *) Handles.valueValueSymbol->raw, RSI, 0);
	// jump to handler
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, ip)), TMP);
	asmJmpq(buffer, TMP);

	asmLabelBind(buffer, &skipBacktrace, asmOffset(buffer));

	// load signaled exception as this stack frame is later destroyed
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), R9);
	// restore SP and BP
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, frame)), RBP);
	asmLeaq(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), RSP);
	// drop the C-side records of the region being cut (see the backtrace path)
	asmPushq(buffer, RAX); // handler
	asmPushq(buffer, R9);  // exception
	asmMovq(buffer, RBP, RDI);
	generateCCall(generator, (intptr_t) unwindThreadStateTo, 1, 0);
	asmPopq(buffer, R9);
	asmPopq(buffer, RAX);
	asmPushq(buffer, R9); // not used argument
	asmPushq(buffer, R9); // push signaled exception
	// load #value: as backtrace is not passed to the handler block
	generateLoadObject(buffer, (RawObject *) Handles.value_Symbol->raw, RSI, 0);
	// jump to handler
	asmMovqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawExceptionHandler, ip)), TMP);
	asmJmpq(buffer, TMP);

	// return to smalltalk code
	asmLabelBind(buffer, &handlerNotFound, asmOffset(buffer));
	asmAddqImm(buffer, RSP, 2 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmMovq(buffer, R13, R11);
}


static void movArg(AssemblerBuffer *buffer, ptrdiff_t index, Register dst)
{
	asmMovqMem(buffer, arg(index), dst);
}


static MemoryOperand arg(ptrdiff_t index)
{
	return asmMem(RSP, NO_REGISTER, SS_1, (index + 1) * sizeof(intptr_t));
}
