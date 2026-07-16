// ppc64le (little-endian, ELFv2) backend, the generated primitives. A COPY of
// vm/jit/ppc64/PrimitivesPpc64.c with ONE structural divergence, called out
// below. Read vm/jit/ppc64/DESIGN.md, then vm/jit/ppc64le/DESIGN.md.
//
// Shared with the BE backend (differences from x64, all pinned in the BE
// DESIGN.md):
//  - frameless primitives: arg i lives at i*8(r1) (POWER keeps the return
//    address in LR, not on the stack); FRAMED primitives push LR first, which
//    makes every FP-relative x64 offset identical;
//  - overflow checks are addo./nego./mulldo. + bso with the sticky-XER[SO]
//    protocol (misses re-arm);
//  - IntMod keeps x64's sign-fixup quirk bug-compatibly.
//
// THE ELFv2 DIVERGENCE: generateCCallPrimitive is NOT fused here. ELFv1 had to
// fuse the C call, the marshalling and the decode into one sequence because it
// returns the 16-byte PrimitiveResult through a hidden sret pointer in r3,
// whose buffer lives inside the ABI frame and therefore must be read back
// BEFORE teardown. ELFv2 returns it in r3:r4 with unshifted arguments (the
// SysV shape), so this backend uses the plain x64 structure: marshal hook,
// R11/R13 dance, generateCCall, decode hook.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vm/jit/ppc64le/ is LITTLE-ENDIAN ppc64le only (ppc64 has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/ppc64le/AssemblerPpc64le.h"
#include "jit/ppc64le/Abi.h"
#include "jit/TargetPrimitives.h"
#include "core/Exception.h"
#include "core/StackFrame.h"
#include "core/CompiledCode.h"
#include "jit/StubCode.h"
#include "jit/CodeDescriptors.h"

static void generateBlockValuePrimitive(CodeGenerator *generator, uint8_t args);

// Frameless primitives: Smalltalk argument i (receiver = 0) sits at i*8(r1).
static void movArg(AssemblerBuffer *buffer, ptrdiff_t index, Register dst)
{
	asmLd(buffer, dst, index * sizeof(intptr_t), R1);
}

// Framed-primitive prologue/epilogue (LR discipline rule 1): push LR first:
// [FP+8] then holds the send-site return address, byte-compatible with the
// x64 call-pushed one, then FP.
static void primFramedPrologue(AssemblerBuffer *buffer)
{
	asmMflr(buffer, R0);
	asmPush(buffer, R0);
	asmPush(buffer, FP);
	asmMr(buffer, FP, R1);
}

// Tear the frame down to FP, restore FP and LR. Emits NO blr: return paths
// add it; fail paths fall through into the method body (whose prologue
// re-pushes the restored LR).
static void primFramedEpilogue(AssemblerBuffer *buffer)
{
	asmMr(buffer, R1, FP);
	asmPop(buffer, FP);
	asmPop(buffer, R0);
	asmMtlr(buffer, R0);
}


// CCALL-primitive trampoline. Structurally the x64 one, NOT the fused ELFv1
// sequence its BE sibling needs: under ELFv2 the PrimitiveResult comes back in
// r3:r4 with unshifted arguments, so there is no sret buffer to read back
// before teardown and generateCCall can own the frame as usual.
//
// Order matters: emitCCallPrimArgs marshals from i*8(r1) while r1 is still the
// primitive's frameless entry SP, and generateCCall (which only touches r0,
// r1, r2, TMP, TGT, FP and CTX) then builds the temp frame around the already
// loaded r3..r10. generateCCall pushes LR before FP, so [FP+8] holds the
// send-site IC exactly as x64's call-pushed return address does, and it
// restores LR before we blr.
void generateCCallPrimitive(CodeGenerator *generator, PrimitiveResult (*cFunction)(), size_t argsSize)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel failed;
	asmInitLabel(&failed);
	ASSERT(argsSize <= 5);

	gPpc64leAbi->emitCCallPrimArgs(buffer, argsSize);
	asmMr(buffer, R15_PPC, TGT);       // the x64 R11->R13 native-code dance
	generateCCall(generator, (intptr_t) cFunction, argsSize, 0);
	gPpc64leAbi->emitPrimResultCheck(buffer, &failed);
	asmBlr(buffer);
	asmPpcLabelBind(buffer, &failed, asmOffset(buffer));
	asmMr(buffer, TGT, R15_PPC);
	// fall through to the Smalltalk fallback (method body)
}


static void loadClass(CodeGenerator *generator, Register src, Register dst)
{
	asmLdT(&generator->buffer, dst, -1, src);
}


// x64 `test reg, 3`, sets CR0 like every andi.; the result lands in the r0
// throwaway.
static void testInt(CodeGenerator *generator, Register reg)
{
	asmAndiDot(&generator->buffer, R0, reg, 3);
}


static void primitveNotImplemented(void)
{
	printf("Error: Primitive is not implemented\n");
	exit(EXIT_FAILURE);
}


void generateNotImplementedPrimitive(CodeGenerator *generator)
{
	// A bare movabs+call on x64; on POWER every C call takes the descriptor
	// discipline, generateCCall (which also preserves LR).
	generateCCall(generator, (intptr_t) primitveNotImplemented, 0, 0);
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

	movArg(buffer, 0, R4);   // receiver
	movArg(buffer, 1, R5);   // index

	testInt(generator, R5);
	asmBne(buffer, &notInt);

	loadClass(generator, R4, R6);
	asmSrdi(buffer, R5, R5, 2);
	asmAddi(buffer, R5, R5, -1);
	asmLbz(buffer, R0, sizeOffset, R6);
	asmCmpld(buffer, 0, R5, R0);
	asmBge(buffer, &outOfBounds);   // index >= varsSize (unsigned: negatives too)

	asmLbz(buffer, R0, isIndexedOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmLbz(buffer, R0, payloadSizeOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmSldi(buffer, R0, R5, 3);
	asmAdd(buffer, TMP2, R4, R0);
	asmLdT(buffer, R3, HEADER_SIZE - 1, TMP2);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmAddi(buffer, R5, R5, 1);
	asmSldi(buffer, R5, R5, 2);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
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

	movArg(buffer, 0, R4);   // receiver
	movArg(buffer, 1, R5);   // index
	movArg(buffer, 2, R3);   // value (also the result)

	testInt(generator, R5);
	asmBne(buffer, &notInt);

	loadClass(generator, R4, R6);
	asmSrdi(buffer, R5, R5, 2);
	asmAddi(buffer, R5, R5, -1);
	asmLbz(buffer, R0, sizeOffset, R6);
	asmCmpld(buffer, 0, R5, R0);
	asmBge(buffer, &outOfBounds);

	asmLbz(buffer, R0, isIndexedOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmLbz(buffer, R0, payloadSizeOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmSldi(buffer, R0, R5, 3);
	asmAdd(buffer, TMP2, R4, R0);
	asmAddi(buffer, TMP2, TMP2, -1);   // untag the folded base once
	asmStd(buffer, R3, HEADER_SIZE, TMP2);
	generateStoreCheck(generator, R4, R3);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmAddi(buffer, R5, R5, 1);
	asmSldi(buffer, R5, R5, 2);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
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

	movArg(buffer, 0, R4);   // receiver
	movArg(buffer, 1, R5);   // index

	testInt(generator, R5);
	asmBne(buffer, &notInt);

	loadClass(generator, R4, R6);
	asmLbz(buffer, R0, isIndexedOffset, R6);
	asmAndiDot(buffer, R0, R0, 1);
	asmBeq(buffer, &notIndexable);

	asmSrdi(buffer, R5, R5, 2);
	asmAddi(buffer, R5, R5, -1);
	asmLdT(buffer, R0, offsetof(RawIndexedObject, size) - 1, R4);
	asmCmpld(buffer, 0, R5, R0);
	asmBge(buffer, &outOfBounds);

	asmLbz(buffer, R0, isBytesOffset, R6);
	asmAndiDot(buffer, R0, R0, 1);
	asmBne(buffer, &bytes);

	// pointers: slot index += payloadSize + varsSize
	asmLbz(buffer, R0, payloadSizeOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmLbz(buffer, R0, sizeOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmSldi(buffer, R0, R5, 3);
	asmAdd(buffer, TMP2, R4, R0);
	asmLdT(buffer, R3, HEADER_SIZE + sizeof(Value) - 1, TMP2);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &bytes, asmOffset(buffer));
	// byte offset = index + (payloadSize + varsSize) * 8 + 8
	asmLbz(buffer, R3, payloadSizeOffset, R6);
	asmLbz(buffer, R0, sizeOffset, R6);
	asmAdd(buffer, R3, R3, R0);
	asmSldi(buffer, R3, R3, 3);
	asmAdd(buffer, R3, R3, R5);
	asmAddi(buffer, R3, R3, sizeof(Value));
	asmAdd(buffer, TMP2, R4, R3);
	asmLbz(buffer, R3, HEADER_SIZE - 1, TMP2);
	asmSldi(buffer, R3, R3, 2);   // tag as SmallInteger
	asmLbz(buffer, R0, valueTypeOffset, R6);
	asmAdd(buffer, R3, R3, R0);   // + value-type tag (int/char)
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmAddi(buffer, R5, R5, 1);
	asmSldi(buffer, R5, R5, 2);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &notIndexable, asmOffset(buffer));
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

	asmInitLabel(&notInt);
	asmInitLabel(&notIndexable);
	asmInitLabel(&outOfBounds);
	asmInitLabel(&bytes);

	movArg(buffer, 0, R4);   // receiver
	movArg(buffer, 1, R5);   // index
	movArg(buffer, 2, R7);   // value

	// value is the result
	asmMr(buffer, R3, R7);

	testInt(generator, R5);
	asmBne(buffer, &notInt);

	loadClass(generator, R4, R6);
	asmLbz(buffer, R0, isIndexedOffset, R6);
	asmAndiDot(buffer, R0, R0, 1);
	asmBeq(buffer, &notIndexable);

	asmSrdi(buffer, R5, R5, 2);
	asmAddi(buffer, R5, R5, -1);

	asmLdT(buffer, R0, offsetof(RawIndexedObject, size) - 1, R4);
	asmCmpld(buffer, 0, R5, R0);
	asmBge(buffer, &outOfBounds);

	asmLbz(buffer, R0, isBytesOffset, R6);
	asmAndiDot(buffer, R0, R0, 1);
	asmBne(buffer, &bytes);

	// pointers: slot index += payloadSize + varsSize, then store + barrier
	asmLbz(buffer, R0, payloadSizeOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmLbz(buffer, R0, sizeOffset, R6);
	asmAdd(buffer, R5, R5, R0);
	asmSldi(buffer, R0, R5, 3);
	asmAdd(buffer, TMP2, R4, R0);
	asmAddi(buffer, TMP2, TMP2, -1);
	asmStd(buffer, R7, HEADER_SIZE + sizeof(Value), TMP2);
	generateStoreCheck(generator, R4, R7);
	asmBlr(buffer);

	// bytes: untag the value, store one byte
	asmPpcLabelBind(buffer, &bytes, asmOffset(buffer));
	asmLbz(buffer, R7, payloadSizeOffset, R6);
	asmLbz(buffer, R0, sizeOffset, R6);
	asmAdd(buffer, R7, R7, R0);
	asmSldi(buffer, R7, R7, 3);
	asmAdd(buffer, R7, R7, R5);
	asmAddi(buffer, R7, R7, sizeof(Value));
	asmAdd(buffer, TMP2, R4, R7);
	asmSrdi(buffer, R0, R3, 2);
	asmStb(buffer, R0, HEADER_SIZE - 1, TMP2);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &outOfBounds, asmOffset(buffer));
	asmAddi(buffer, R5, R5, 1);
	asmSldi(buffer, R5, R5, 2);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &notIndexable, asmOffset(buffer));
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

	movArg(buffer, 0, R4);

	asmAndiDot(buffer, R0, R4, 3);
	asmCmpldi(buffer, 0, R0, VALUE_POINTER);
	asmBne(buffer, &notPointer);

	loadClass(generator, R4, R5);
	asmLbz(buffer, R0, isIndexedOffset, R5);
	asmAndiDot(buffer, R0, R0, 1);
	asmBeq(buffer, &notIndexable);

	asmLdT(buffer, R3, offsetof(RawIndexedObject, size) - 1, R4);
	asmSldi(buffer, R3, R3, 2);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notPointer, asmOffset(buffer));
	asmPpcLabelBind(buffer, &notIndexable, asmOffset(buffer));
	asmLi(buffer, R3, 0);
	asmBlr(buffer);
}


void generateIdentityPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel equal;
	asmInitLabel(&equal);

	movArg(buffer, 0, R4);
	movArg(buffer, 1, R5);
	asmCmpd(buffer, 0, R4, R5);
	asmBeq(buffer, &equal);
	generateLoadObject(buffer, Handles.false->raw, R3, 1);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &equal, asmOffset(buffer));
	generateLoadObject(buffer, Handles.true->raw, R3, 1);
	asmBlr(buffer);
}


void generateHashPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, R4);
	// hash is the u32 at byte offset 8 and is NOT endian-mirrored (only
	// CompiledCodeHeader is), so a 32-bit load at its offset is endian-correct
	// by construction (x64 masks a 64-bit load instead). r4 holds the TAGGED
	// receiver, hence varOffset: EA = (obj+1) + 7 = obj+8, bytes 8-11.
	// (This read used to be one byte high, at obj+9, which stayed invisible
	// because a wrong-but-deterministic hash still behaves like a hash: it just
	// dropped the top 8 bits of entropy and disagreed with C's obj->hash.)
	asmLwz(buffer, R3, varOffset(RawObject, hash), R4);
	asmSldi(buffer, R3, R3, 2);
	asmBlr(buffer);
}


void generateClassPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, R4);
	generateLoadClass(buffer, R4, R3);
	asmBlr(buffer);
}


void generateBehaviorNewPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;

	primFramedPrologue(buffer);

	// save native code + dummy context (frame slots 0/1)
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	asmLd(buffer, R4, 2 * sizeof(intptr_t), FP);   // receiver = the class
	asmAddi(buffer, R4, R4, -1);                   // untag
	asmLi(buffer, R5, 0);
	generateStubCall(generator, &AllocateStub);

	primFramedEpilogue(buffer);
	asmBlr(buffer);
}


void generateBehaviorNewSizePrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	primFramedPrologue(buffer);

	// save native code + dummy context
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	asmLd(buffer, R5, 3 * sizeof(intptr_t), FP);   // size argument

	testInt(generator, R5);
	asmBne(buffer, &notInt);

	asmLd(buffer, R4, 2 * sizeof(intptr_t), FP);
	asmAddi(buffer, R4, R4, -1);
	asmSrdi(buffer, R5, R5, 2);
	generateStubCall(generator, &AllocateStub);

	primFramedEpilogue(buffer);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	primFramedEpilogue(buffer);
	// fall through to the Smalltalk fallback
}


void generateCharacterNewPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, R3);
	testInt(generator, R3);
	asmBne(buffer, &notInt);

	asmAddi(buffer, R3, R3, VALUE_CHAR);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateCharacterCodePrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, R3);
	asmAddi(buffer, R3, R3, -VALUE_CHAR);
	asmBlr(buffer);
}


void generateStringHashPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	movArg(buffer, 0, R3);
	asmAddi(buffer, R3, R3, -1);
	// bare call on x64; POWER takes the full descriptor discipline (LR is
	// preserved by generateCCall's stash, so the frameless blr stays valid)
	generateCCall(generator, (intptr_t) computeRawStringHash, 1, 0);
	asmSldi(buffer, R3, R3, 2);
	asmBlr(buffer);
}


void generateInterruptPrimitive(CodeGenerator *generator)
{
	asmTrap(&generator->buffer);
}


void generateExitPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	asmLi(buffer, R3, 1);
	generateCCall(generator, (intptr_t) exit, 1, 0);
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

	primFramedPrologue(buffer);

	// save native code + dummy context
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	// scopedGetNativeCode(receiver, method)
	asmLd(buffer, R4, 2 * sizeof(intptr_t), FP);   // method
	asmLd(buffer, R3, 3 * sizeof(intptr_t), FP);   // receiver
	generateCCall(generator, (intptr_t) scopedGetNativeCode, 2, 1);
	// push receiver
	asmLd(buffer, R0, 3 * sizeof(intptr_t), FP);
	asmPush(buffer, R0);
	// invoke method
	asmMr(buffer, TGT, R3);
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// epilogue (drops the 3 pushed slots too)
	primFramedEpilogue(buffer);
	asmBlr(buffer);
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

	primFramedPrologue(buffer);

	// save native code + dummy context
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	// scopedGetNativeCode(receiver, method)
	asmLd(buffer, R4, 2 * sizeof(intptr_t), FP);
	asmLd(buffer, R3, 3 * sizeof(intptr_t), FP);
	generateCCall(generator, (intptr_t) scopedGetNativeCode, 2, 1);

	// load arguments array + its size, check against the method's argsSize
	asmLd(buffer, R6, 4 * sizeof(intptr_t), FP);
	asmLdT(buffer, R9_PPC, varOffset(RawArray, size), R6);
	asmLd(buffer, TMP, 2 * sizeof(intptr_t), FP);
	asmLbz(buffer, R0, argsOffset, TMP);
	asmCmpd(buffer, 0, R0, R9_PPC);
	asmBne(buffer, &invalidArgs);

	// push arguments (last first)
	asmCmpdi(buffer, 0, R9_PPC, 0);
	asmBeq(buffer, &zeroArgs);
	asmPpcLabelBind(buffer, &loop, asmOffset(buffer));
	asmAddicDot(buffer, R9_PPC, R9_PPC, -1);
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, TMP2, R6, R0);
	asmLdT(buffer, TMP, varOffset(RawArray, vars), TMP2);
	asmPush(buffer, TMP);
	asmBne(buffer, &loop);
	asmPpcLabelBind(buffer, &zeroArgs, asmOffset(buffer));

	// push receiver
	asmLd(buffer, R0, 3 * sizeof(intptr_t), FP);
	asmPush(buffer, R0);

	// invoke method
	asmMr(buffer, TGT, R3);
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// epilogue
	primFramedEpilogue(buffer);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &invalidArgs, asmOffset(buffer));
	primFramedEpilogue(buffer);
}


void generateIntLessThanPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel less;
	AssemblerLabel notInt;
	asmInitLabel(&less);
	asmInitLabel(&notInt);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R3);
	asmCmpd(buffer, 0, R3, R4);

	asmBlt(buffer, &less);
	generateLoadObject(buffer, Handles.false->raw, R3, 1);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &less, asmOffset(buffer));
	generateLoadObject(buffer, Handles.true->raw, R3, 1);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntAddPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel overflow;
	asmInitLabel(&notInt);
	asmInitLabel(&overflow);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R3);
	asmAddoDot(buffer, R3, R3, R4);
	asmBso(buffer, &overflow);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &overflow, asmOffset(buffer));
	asmClearXerSo(buffer);   // re-arm the sticky SO before the fallback
}


void generateIntSubPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel overflow;
	asmInitLabel(&notInt);
	asmInitLabel(&overflow);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R3);
	asmSubfoDot(buffer, R3, R4, R3);   // r3 = r3 - r4
	asmBso(buffer, &overflow);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &overflow, asmOffset(buffer));
	asmClearXerSo(buffer);
}


void generateIntMulPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel overflow;
	asmInitLabel(&notInt);
	asmInitLabel(&overflow);

	movArg(buffer, 1, R3);
	testInt(generator, R3);
	asmBne(buffer, &notInt);

	// untag one side (arithmetic shift, x64 used a logical one; same
	// product mod 2^64, cleaner overflow semantics), multiply by the tagged
	// other side: (b>>2) * (a<<2) = (a*b)<<2, still tagged
	asmSradi(buffer, R3, R3, 2);
	movArg(buffer, 0, R4);
	asmMulldoDot(buffer, R3, R3, R4);
	asmBso(buffer, &overflow);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &overflow, asmOffset(buffer));
	asmClearXerSo(buffer);
}


void generateIntQuoPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel divZero;
	asmInitLabel(&notInt);
	asmInitLabel(&divZero);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);
	asmCmpdi(buffer, 0, R4, 0);        // POWER divd does not trap on /0:
	asmBeq(buffer, &divZero);          // fail so the Smalltalk fallback raises

	movArg(buffer, 0, R3);
	asmDivd(buffer, R3, R3, R4);       // tagged/tagged = untagged quotient
	asmSldi(buffer, R3, R3, 2);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &divZero, asmOffset(buffer));
}


void generateIntModPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel negativeResult;
	AssemblerLabel divZero;
	asmInitLabel(&notInt);
	asmInitLabel(&negativeResult);
	asmInitLabel(&divZero);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);
	asmCmpdi(buffer, 0, R4, 0);
	asmBeq(buffer, &divZero);

	movArg(buffer, 0, R3);
	asmDivd(buffer, R5, R3, R4);       // q = a / b
	asmMulld(buffer, R5, R5, R4);      // q * b
	asmSubf(buffer, R3, R5, R3);       // rem = a - q*b (tagged)

	// x64 sign fixup, kept bug-compatible: rem ^ divisor < 0 -> rem += divisor
	asmXor(buffer, R0, R3, R4);
	asmCmpdi(buffer, 0, R0, 0);
	asmBlt(buffer, &negativeResult);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &negativeResult, asmOffset(buffer));
	asmAdd(buffer, R3, R3, R4);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &divZero, asmOffset(buffer));
}


void generateIntRemPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel divZero;
	asmInitLabel(&notInt);
	asmInitLabel(&divZero);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);
	asmCmpdi(buffer, 0, R4, 0);
	asmBeq(buffer, &divZero);

	movArg(buffer, 0, R3);
	asmDivd(buffer, R5, R3, R4);
	asmMulld(buffer, R5, R5, R4);
	asmSubf(buffer, R3, R5, R3);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &divZero, asmOffset(buffer));
}


void generateIntNegPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel overflow;
	asmInitLabel(&overflow);

	movArg(buffer, 0, R3);
	asmNegoDot(buffer, R3, R3);
	asmBso(buffer, &overflow);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &overflow, asmOffset(buffer));
	asmClearXerSo(buffer);
}


void generateIntAndPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, R3);
	testInt(generator, R3);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R4);
	asmAnd(buffer, R3, R3, R4);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntOrPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, R3);
	testInt(generator, R3);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R4);
	asmOr(buffer, R3, R3, R4);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntXorPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	asmInitLabel(&notInt);

	movArg(buffer, 1, R3);
	testInt(generator, R3);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R4);
	asmXor(buffer, R3, R3, R4);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
}


void generateIntShiftPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel notInt;
	AssemblerLabel rightShift;
	asmInitLabel(&notInt);
	asmInitLabel(&rightShift);

	movArg(buffer, 1, R4);
	testInt(generator, R4);
	asmBne(buffer, &notInt);

	movArg(buffer, 0, R3);
	asmSradi(buffer, R4, R4, 2);
	asmCmpdi(buffer, 0, R4, 0);
	asmBlt(buffer, &rightShift);
	asmSld(buffer, R3, R3, R4);        // amounts >= 64 yield 0 (POWER rule)
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &rightShift, asmOffset(buffer));
	asmNeg(buffer, R4, R4);
	asmSrd(buffer, R3, R3, R4);
	asmRldicr(buffer, R3, R3, 0, 61);  // clear the tag bits (& ~3)
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &notInt, asmOffset(buffer));
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
	ptrdiff_t argsOffset = offsetof(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, argsSize) - 1;

	asmInitLabel(&invalidArgs);

	primFramedPrologue(buffer);

	// save native code + current context
	asmPush(buffer, TGT);
	asmPush(buffer, CTX);

	// load compiled block, check the argument count
	asmLd(buffer, TMP, 2 * sizeof(intptr_t), FP);
	asmLdT(buffer, TMP, varOffset(RawBlock, compiledBlock), TMP);
	asmLbz(buffer, R0, argsOffset, TMP);
	asmCmpldi(buffer, 0, R0, args);
	asmBne(buffer, &invalidArgs);

	generateBlockContextAllocation(generator);   // leaves the block in r6

	// epilogue, then tail-jump into the block (LR restored so the block's
	// prologue re-pushes the ORIGINAL send-site return address)
	primFramedEpilogue(buffer);

	// replace the receiver on the stack with the block's captured one
	asmLdT(buffer, TMP, varOffset(RawBlock, receiver), R6);
	asmStd(buffer, TMP, 0, R1);

	// jump to the block's code
	asmLdT(buffer, TGT, varOffset(RawBlock, nativeCode), R6);
	asmAddi(buffer, TGT, TGT, offsetof(NativeCode, insts));
	asmJumpReg(buffer, TGT);

	asmPpcLabelBind(buffer, &invalidArgs, asmOffset(buffer));
	primFramedEpilogue(buffer);
	// fall through to the Smalltalk fallback
}


void generateBlockValueArgsPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel invalidArgs;
	AssemblerLabel loop;
	AssemblerLabel zeroArgs;
	ptrdiff_t argsOffset = offsetof(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, argsSize) - 1;

	asmInitLabel(&invalidArgs);
	asmInitLabel(&loop);
	asmInitLabel(&zeroArgs);

	primFramedPrologue(buffer);

	// save native code + current context
	asmPush(buffer, TGT);
	asmPush(buffer, CTX);

	// block (r15: survives the block-context allocation) + args array
	asmLd(buffer, R15_PPC, 2 * sizeof(intptr_t), FP);
	asmLd(buffer, R7, 3 * sizeof(intptr_t), FP);

	// compiled block + array size, check the argument count
	asmLdT(buffer, TMP, varOffset(RawBlock, compiledBlock), R15_PPC);
	asmLdT(buffer, R9_PPC, varOffset(RawArray, size), R7);
	asmLbz(buffer, R0, argsOffset, TMP);
	asmCmpd(buffer, 0, R0, R9_PPC);
	asmBne(buffer, &invalidArgs);

	// push arguments (last first)
	asmCmpdi(buffer, 0, R9_PPC, 0);
	asmBeq(buffer, &zeroArgs);
	asmPpcLabelBind(buffer, &loop, asmOffset(buffer));
	asmAddicDot(buffer, R9_PPC, R9_PPC, -1);
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, TMP2, R7, R0);
	asmLdT(buffer, TMP, varOffset(RawArray, vars), TMP2);
	asmPush(buffer, TMP);
	asmBne(buffer, &loop);
	asmPpcLabelBind(buffer, &zeroArgs, asmOffset(buffer));

	// push the block's captured receiver
	asmLdT(buffer, R0, varOffset(RawBlock, receiver), R15_PPC);
	asmPush(buffer, R0);

	generateBlockContextAllocation(generator);

	// call the block (through TGT, the VM convention, unlike x64's bare
	// RAX call, so the callee's slot-0 native-code spill stays correct)
	asmLdT(buffer, TGT, varOffset(RawBlock, nativeCode), R15_PPC);
	asmAddi(buffer, TGT, TGT, offsetof(NativeCode, insts));
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// epilogue
	primFramedEpilogue(buffer);
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &invalidArgs, asmOffset(buffer));
	primFramedEpilogue(buffer);
}


void generateBlockWhileTrue(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel loop;
	AssemblerLabel notBoolean;
	asmInitLabel(&loop);
	asmInitLabel(&notBoolean);

	primFramedPrologue(buffer);

	// save native code + dummy context
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, R3, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, R4, 0);
	generateMethodLookup(generator);

	asmPush(buffer, TGT);              // spill the looked-up #value code
	asmAddi(buffer, R1, R1, -8);       // the receiver slot rewritten each pass

	// value block
	asmPpcLabelBind(buffer, &loop, asmOffset(buffer));
	// copy receiver again (block-value replaces it on the stack)
	asmLd(buffer, TMP, 2 * sizeof(intptr_t), FP);
	asmStd(buffer, TMP, 0, R1);
	// reload the #value code and the STABLE context (see x64 rationale)
	asmLd(buffer, TGT, sizeof(intptr_t), R1);
	asmLd(buffer, CTX, -2 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// repeat while true
	generateLoadObject(buffer, Handles.true->raw, TMP, 1);
	asmCmpd(buffer, 0, R3, TMP);
	asmBeq(buffer, &loop);

	// must be false, otherwise fall back
	generateLoadObject(buffer, Handles.false->raw, TMP, 1);
	asmCmpd(buffer, 0, R3, TMP);
	asmBne(buffer, &notBoolean);

	// return the receiver
	asmLd(buffer, R3, 2 * sizeof(intptr_t), FP);
	primFramedEpilogue(buffer);
	asmBlr(buffer);

	// not boolean: skip receiver slot + #value spill + dummy context, pop
	// the ORIGINAL native code back into TGT (x64: addq 3*8; pop R11), fall
	// through to the Smalltalk fallback
	asmPpcLabelBind(buffer, &notBoolean, asmOffset(buffer));
	asmDropStack(buffer, 3);
	asmPop(buffer, TGT);
	primFramedEpilogue(buffer);
}


void generateBlockWhileTrue2(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel loop;
	AssemblerLabel end;
	AssemblerLabel notBoolean;
	asmInitLabel(&loop);
	asmInitLabel(&end);
	asmInitLabel(&notBoolean);

	primFramedPrologue(buffer);

	// save native code + dummy context
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, R3, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, R4, 0);
	generateMethodLookup(generator);
	asmPush(buffer, TGT);              // spill the looked-up #value code

	// value the condition block
	asmPpcLabelBind(buffer, &loop, asmOffset(buffer));
	asmLd(buffer, R0, 2 * sizeof(intptr_t), FP);
	asmPush(buffer, R0);
	asmLd(buffer, TGT, sizeof(intptr_t), R1);
	asmLd(buffer, CTX, -2 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmCallReg(buffer, TGT);
	generateStackmap(generator);
	asmDropStack(buffer, 1);

	// repeat while true
	generateLoadObject(buffer, Handles.true->raw, TMP, 1);
	asmCmpd(buffer, 0, R3, TMP);
	asmBne(buffer, &end);

	// value the body block
	asmLd(buffer, R0, 3 * sizeof(intptr_t), FP);
	asmPush(buffer, R0);
	asmLd(buffer, TGT, sizeof(intptr_t), R1);
	asmLd(buffer, CTX, -2 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmCallReg(buffer, TGT);
	generateStackmap(generator);
	asmDropStack(buffer, 1);
	asmB(buffer, &loop);

	// result is not true
	asmPpcLabelBind(buffer, &end, asmOffset(buffer));

	generateLoadObject(buffer, Handles.false->raw, TMP, 1);
	asmCmpd(buffer, 0, R3, TMP);
	asmBne(buffer, &notBoolean);

	// return the receiver
	asmLd(buffer, R3, 2 * sizeof(intptr_t), FP);
	primFramedEpilogue(buffer);
	asmBlr(buffer);

	// not boolean: skip #value spill + dummy context, pop the ORIGINAL
	// native code back into TGT (x64: addq 2*8; pop R11), fall through
	asmPpcLabelBind(buffer, &notBoolean, asmOffset(buffer));
	asmDropStack(buffer, 2);
	asmPop(buffer, TGT);
	primFramedEpilogue(buffer);
}


void generateBlockOnExceptionPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerFixup *ip;
	generator->frameSize = 2;

	// framed prologue with local slots
	primFramedPrologue(buffer);
	asmAddi(buffer, R1, R1, -(ptrdiff_t) (generator->frameSize * sizeof(intptr_t)));

	// save native code
	asmStd(buffer, TGT, -(ptrdiff_t) sizeof(intptr_t), FP);
	generateMethodContextAllocation(generator, 0);

	// allocate exception handler
	generateLoadObject(buffer, (RawObject *) Handles.ExceptionHandler->raw, R4, 0);
	asmLi(buffer, R5, 0);
	generateStubCall(generator, &AllocateStub);
	// setup return IP (an absolute address inside THIS code object: a li64
	// immediate patched at build time through the code-pointer fixup)
	asmLi64(buffer, TMP, 0);
	ip = asmEmitFixup(buffer, ASM_FIXUP_IP, ASM_FIXUP_SIZE_CODE_POINTER, asmOffset(buffer) - 20);
	asmStdT(buffer, TMP, varOffset(RawExceptionHandler, ip), R3);
	// setup context exception handler
	asmLd(buffer, R8_PPC, -2 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmStdT(buffer, R8_PPC, varOffset(RawExceptionHandler, context), R3);
	// spill exception handler
	asmPush(buffer, R3);
	generator->frameSize++;

	// install into the RUNNING worker's chain (see x64 rationale)
	asmLoadTls(buffer, R6, gCurrentThreadTpoff);
	asmLd(buffer, TMP, offsetof(Thread, exceptionHandler), R6);
	asmStdT(buffer, TMP, varOffset(RawExceptionHandler, parent), R3);
	asmStd(buffer, R3, offsetof(Thread, exceptionHandler), R6);

	// value block
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, R3, 1);
	generateLoadObject(buffer, (RawObject *) Handles.valueSymbol->raw, R4, 0);
	asmLd(buffer, R0, 2 * sizeof(intptr_t), FP);
	asmPush(buffer, R0);
	generator->frameSize++;
	generateMethodLookup(generator);
	generator->frameSize--;
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// unregister the handler (restore parent into the RUNNING worker's chain)
	asmLd(buffer, TMP, -3 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmLdT(buffer, TMP, varOffset(RawExceptionHandler, parent), TMP);
	asmLoadTls(buffer, R6, gCurrentThreadTpoff);
	asmStd(buffer, TMP, offsetof(Thread, exceptionHandler), R6);

	// epilogue
	asmLdT(buffer, CTX, varOffset(RawContext, parent), CTX);
	primFramedEpilogue(buffer);
	asmBlr(buffer);

	// jumped from exception signal (FP/r1 rebuilt by the signal path)
	ip->value = asmOffset(buffer) - ip->offset;
	generator->frameSize = 5; // native code + context + handler + backtrace + exception

	// restore context
	asmLd(buffer, CTX, -2 * (ptrdiff_t) sizeof(intptr_t), FP);

	// value the exception block (the signal path pre-loaded r4 with the
	// #value:/#value:value: selector and pushed the handler args)
	asmLd(buffer, R0, 4 * sizeof(intptr_t), FP);
	asmPush(buffer, R0);
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, R3, 1);
	generateMethodLookup(generator);
	generator->frameSize -= 3;
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// epilogue
	asmLdT(buffer, CTX, varOffset(RawContext, parent), CTX);
	primFramedEpilogue(buffer);
	asmBlr(buffer);
}


void generateExceptionSignalPrimitive(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel handlerNotFound;
	AssemblerLabel skipBacktrace;
	asmInitLabel(&handlerNotFound);
	asmInitLabel(&skipBacktrace);

	primFramedPrologue(buffer);

	// save native code + dummy context
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	asmMr(buffer, R15_PPC, TGT);   // survives the C call (x64 R13)

	asmLd(buffer, R3, 2 * sizeof(intptr_t), FP);
	asmAddi(buffer, R3, R3, -1);

	generateCCall(generator, (intptr_t) unwindExceptionHandler, 1, 1);
	asmCmpdi(buffer, 0, R3, 0);
	asmBeq(buffer, &handlerNotFound);

	// handler found: r3 = handler (raw). Load its context and the handler
	// block, decide whether the block wants a backtrace argument.
	asmLdT(buffer, CTX, varOffset(RawExceptionHandler, context), R3);
	asmLdT(buffer, TMP, varOffset(RawContext, frame), CTX);
	asmLd(buffer, TMP, 4 * sizeof(intptr_t), TMP);
	asmLdT(buffer, TMP, varOffset(RawBlock, compiledBlock), TMP);
	ptrdiff_t argsSizeOffset = varOffset(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, argsSize);
	asmLbz(buffer, R0, argsSizeOffset, TMP);
	asmCmpldi(buffer, 0, R0, 2);
	asmBlt(buffer, &skipBacktrace);

	asmPush(buffer, R3);           // spill the handler
	generator->frameSize++;

	// generate the backtrace: exception generateBacktrace
	asmLd(buffer, TMP, 2 * sizeof(intptr_t), FP);
	asmPush(buffer, TMP);
	generator->frameSize++;
	generateLoadClass(buffer, TMP, R3);
	generateLoadObject(buffer, (RawObject *) Handles.generateBacktraceSymbol->raw, R4, 0);
	generateMethodLookup(generator);
	generator->frameSize--;
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	asmDropStack(buffer, 1);
	asmPop(buffer, R6);            // the spilled handler
	generator->frameSize--;

	// load the signaled exception (this frame is about to be destroyed)
	asmLd(buffer, R8_PPC, 2 * sizeof(intptr_t), FP);
	// rebuild FP/r1 at the handler's frame
	asmLdT(buffer, CTX, varOffset(RawExceptionHandler, context), R6);
	asmLdT(buffer, FP, varOffset(RawContext, frame), CTX);
	asmAddi(buffer, R1, FP, -2 * (ptrdiff_t) sizeof(intptr_t));
	asmPush(buffer, R3);           // generated backtrace
	asmPush(buffer, R8_PPC);       // signaled exception
	// #value:value: (backtrace not passed to the handler block itself)
	generateLoadObject(buffer, (RawObject *) Handles.valueValueSymbol->raw, R4, 0);
	// jump into on:do: at the handler's return IP
	asmLdT(buffer, TMP, varOffset(RawExceptionHandler, ip), R6);
	asmJumpReg(buffer, TMP);

	asmPpcLabelBind(buffer, &skipBacktrace, asmOffset(buffer));

	// load the signaled exception, rebuild FP/r1 at the handler's frame
	asmLd(buffer, R8_PPC, 2 * sizeof(intptr_t), FP);
	asmLdT(buffer, FP, varOffset(RawContext, frame), CTX);
	asmAddi(buffer, R1, FP, -2 * (ptrdiff_t) sizeof(intptr_t));
	asmPush(buffer, R8_PPC);       // unused second argument slot
	asmPush(buffer, R8_PPC);       // signaled exception
	generateLoadObject(buffer, (RawObject *) Handles.value_Symbol->raw, R4, 0);
	asmLdT(buffer, TMP, varOffset(RawExceptionHandler, ip), R3);
	asmJumpReg(buffer, TMP);

	// no handler: return to the Smalltalk fallback
	asmPpcLabelBind(buffer, &handlerNotFound, asmOffset(buffer));
	asmDropStack(buffer, 2);
	primFramedEpilogue(buffer);
	asmMr(buffer, TGT, R15_PPC);
}
