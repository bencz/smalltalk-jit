// ppc64le (little-endian, ELFv2) backend, the four JIT stubs. A deliberate
// COPY of vm/jit/ppc64/StubCodePpc64.c: the register/frame mapping is
// identical (FP=r31, CTX=r30, TGT=r12, TMP=r11, TMP2=r10, r3=result/argA, LR
// discipline, tagged-base asmLdT/asmStdT), and so are the object-header
// stores, which are natural-width accesses at each field's own offset and
// therefore endian-neutral. Read vm/jit/ppc64/DESIGN.md, then
// vm/jit/ppc64le/DESIGN.md for the deltas.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vm/jit/ppc64le/ is LITTLE-ENDIAN ppc64le only (ppc64 has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/StubCode.h"
#include "memory/Heap.h"
#include "core/Lookup.h"
#include "jit/CodeGenerator.h"
#include "jit/CodeDescriptors.h"
#include "jit/ppc64le/AssemblerPpc64le.h"
#include "jit/ppc64le/Abi.h"
#include "core/Thread.h"
#include "core/StackFrame.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"

static CompiledMethod *createDoesNotUnderstandCode(void);


// Direct JIT-to-JIT call into a stub (raw insts address, descriptors are a
// C-ABI affair only). LR: callers are method bodies (LR dead) or framed
// primitives (LR pushed at entry), see the LR discipline in DESIGN.md.
void generateStubCall(CodeGenerator *generator, StubCode *stubCode)
{
	AssemblerBuffer *buffer = &generator->buffer;
	asmLi64(buffer, TMP, (uint64_t) getStubNativeCode(stubCode)->insts);
	asmCallReg(buffer, TMP);
	generateStackmap(generator);
	if (generator->descriptors != NULL) {
		ordCollAdd(generator->descriptors, createBytecodeDescriptor(asmOffset(buffer), generator->bytecodeNumber));
	}
}


// The C -> Smalltalk entry trampoline. Called through the ELFv2 seam
// (targetCallSmalltalkEntry builds the descriptor), so the IN-args arrive in
// the C argument registers r3=method, r4=nativeCode, r5=args, r6=thread.
// The entry hooks save/restore the full nonvolatile frame (LR/CR included);
// everything between is the VM-internal EntryStackFrame protocol.
static void generateSmalltalkEntry(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	const Ppc64leAbi *abi = &AbiPpc64leElfV2;
	AssemblerLabel loop;
	ptrdiff_t argsOffset = offsetof(RawCompiledMethod, header) + offsetof(CompiledCodeHeader, argsSize);

	Register regMethod = (Register) abi->argRegs[0];   // r3
	Register regEntry = (Register) abi->argRegs[1];    // r4
	Register regArgs = (Register) abi->argRegs[2];     // r5
	Register regThread = (Register) abi->argRegs[3];   // r6

	size_t savedRegsSize = (size_t) abi->entrySavedRegsSize;
	size_t frameSize = align(savedRegsSize + sizeof(EntryStackFrame), 16);
	frameSize -= savedRegsSize;

	asmInitLabel(&loop);

	abi->emitEntrySaveRegs(buffer); // lowers r1 by exactly entrySavedRegsSize

	asmMr(buffer, FP, R1);
	asmAddi(buffer, R1, R1, -(ptrdiff_t) frameSize);

	// load previous entry frame
	asmLd(buffer, TMP, offsetof(Thread, stackFramesTail), regThread);
	// setup it in current entry frame
	asmStd(buffer, TMP, offsetof(EntryStackFrame, prev), R1);
	// setup FP
	asmStd(buffer, FP, offsetof(EntryStackFrame, entry), R1);
	// zero exit frame
	asmLi(buffer, R0, 0);
	asmStd(buffer, R0, offsetof(EntryStackFrame, exit), R1);
	// setup entry stack frame to thread
	asmStd(buffer, R1, offsetof(Thread, stackFramesTail), regThread);

	// load method arguments size (r14: VM-internal loop counter, inside the
	// entry hook's saved area, the C caller's r14 is preserved by the hook)
	asmLbz(buffer, R14_PPC, argsOffset, regMethod);
	asmAddi(buffer, R14_PPC, R14_PPC, 1); // there is always the 'self' argument
	asmPush(buffer, R14_PPC);

	// push arguments on stack, last first (args[count-1] .. args[0])
	asmPpcLabelBind(buffer, &loop, asmOffset(buffer));
	asmSldi(buffer, R0, R14_PPC, 3);
	asmAdd(buffer, R9_PPC, regArgs, R0);
	asmLd(buffer, R9_PPC, -8, R9_PPC);
	asmPush(buffer, R9_PPC);
	asmAddicDot(buffer, R14_PPC, R14_PPC, -1);
	asmBne(buffer, &loop);

	// load context
	asmLd(buffer, CTX, offsetof(Thread, context), regThread);

	// invoke method (VM convention: native-code entry arrives in TGT=r12)
	asmMr(buffer, TGT, regEntry);
	asmCallReg(buffer, regEntry);

	// load the CURRENT worker's thread from TLS (the fiber may have migrated
	// OS threads during the Smalltalk call); regThread doubles as the
	// post-call scratch, volatile in the C ABI
	asmLoadTls(buffer, regThread, gCurrentThreadTpoff);
	// load entry frame
	asmLd(buffer, TMP, offsetof(Thread, stackFramesTail), regThread);
	// load previous entry frame
	asmLd(buffer, TMP, offsetof(EntryStackFrame, prev), TMP);
	// setup entry stack frame to thread
	asmStd(buffer, TMP, offsetof(Thread, stackFramesTail), regThread);

	// restore r1: pop args (count reloaded from its slot below the entry
	// frame area) plus the count slot and the EntryStackFrame area
	asmLd(buffer, R14_PPC, -(ptrdiff_t) (sizeof(intptr_t) + frameSize), FP);
	asmSldi(buffer, R0, R14_PPC, 3);
	asmAdd(buffer, R1, R1, R0);
	asmAddi(buffer, R1, R1, sizeof(intptr_t) + frameSize);

	abi->emitEntryRestoreRegs(buffer);
	asmBlr(buffer);
}
StubCode SmalltalkEntry = { .generator = generateSmalltalkEntry, .id = STUB_SMALLTALK_ENTRY };


static void generateAllocate(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel noFreeSpace;
	AssemblerLabel bytes;
	AssemblerLabel alignLabel;
	AssemblerLabel notIndexed;
	AssemblerLabel noPayload;
	AssemblerLabel payloadLoop;
	AssemblerLabel noVars;
	AssemblerLabel varsLoop;
	AssemblerLabel noBytes;
	AssemblerLabel zeroBytes;
	AssemblerLabel zeroBytesLoop;

	ptrdiff_t sizeOffset = offsetof(RawClass, instanceShape) + offsetof(InstanceShape, size);
	ptrdiff_t varsOffset = offsetof(RawClass, instanceShape) + offsetof(InstanceShape, varsSize);
	ptrdiff_t isBytesOffset = offsetof(RawClass, instanceShape) + offsetof(InstanceShape, isBytes);
	ptrdiff_t tlabOffset = offsetof(Thread, tlab);
	ptrdiff_t payloadOffset = offsetof(RawClass, instanceShape) + offsetof(InstanceShape, payloadSize);
	ptrdiff_t isIndexedOffset = offsetof(RawClass, instanceShape) + offsetof(InstanceShape, isIndexed);

	asmInitLabel(&noFreeSpace);
	asmInitLabel(&bytes);
	asmInitLabel(&alignLabel);
	asmInitLabel(&notIndexed);
	asmInitLabel(&noPayload);
	asmInitLabel(&payloadLoop);
	asmInitLabel(&noVars);
	asmInitLabel(&varsLoop);
	asmInitLabel(&noBytes);
	asmInitLabel(&zeroBytes);
	asmInitLabel(&zeroBytesLoop);

	// IN:  r4 = class (raw), r5 = indexed variables size (untagged count)
	// OUT: r3 = new object, tagged
	// Register budget (DESIGN.md): r6 = instance size, r7 = varsSize,
	// r9 = thread base until the bump/slow-path branch, then loop counter;
	// r8 = untagged cursor base; r6 repurposed as the inst-vars pointer once
	// the size is dead.

	// load instance and variables size
	asmLhz(buffer, R6, sizeOffset, R4);   // r6: instance size
	asmLbz(buffer, R7, varsOffset, R4);   // r7: instance variables size

	// add indexed fields size
	asmLbz(buffer, R0, isBytesOffset, R4);
	asmAndiDot(buffer, R0, R0, 1);
	asmBne(buffer, &bytes);

	// pointers shape
	asmSldi(buffer, R0, R5, 3);
	asmAdd(buffer, R6, R6, R0);           // instance size += indexed * 8
	asmAdd(buffer, R7, R7, R5);           // vars size += indexed
	asmB(buffer, &alignLabel);

	// bytes shape
	asmPpcLabelBind(buffer, &bytes, asmOffset(buffer));
	asmAdd(buffer, R6, R6, R5);           // instance size += indexed

	// align
	asmPpcLabelBind(buffer, &alignLabel, asmOffset(buffer));
	asmAddi(buffer, R6, R6, HEAP_OBJECT_ALIGN - 1);
	asmRldicr(buffer, R6, R6, 0, 59);     // r6: aligned size (& ~15)

	// check free space, bump the RUNNING worker's TLAB, read from TLS so
	// shared JIT code allocates into whichever worker executes this fiber
	asmLoadTls(buffer, R9_PPC, gCurrentThreadTpoff); // r9: &CurrentThread
	asmLd(buffer, TMP, tlabOffset + offsetof(TLAB, end), R9_PPC);
	asmLd(buffer, R3, tlabOffset + offsetof(TLAB, top), R9_PPC); // r3: new object
	asmSubf(buffer, TMP, R3, TMP);        // TMP = end - top (free space)
	asmCmpld(buffer, 0, R6, TMP);
	asmBgt(buffer, &noFreeSpace);         // size > free -> slow path

	// move top cursor: [tlab.top] += size
	asmLd(buffer, TMP2, tlabOffset + offsetof(TLAB, top), R9_PPC);
	asmAdd(buffer, TMP2, TMP2, R6);
	asmStd(buffer, TMP2, tlabOffset + offsetof(TLAB, top), R9_PPC);

	// class
	asmStd(buffer, R4, offsetof(RawObject, class), R3);
	// header: hash = (addr >> 2) truncated to 32 bits. Two 32-bit stores, the
	// hash word at +8 and a zeroed unused/payloadSize/varsSize/tags word at
	// +12. Natural-width stores at each field's own offset are endian-NEUTRAL
	// (identical here and in the BE backend), and semantically identical to
	// x64's single 64-bit store of the 0xFFFFFFFF-masked hash, which lands the
	// hash in bytes 8-11 and zeros 12-15. It is x64's form that is
	// little-endian-specific, not this one that is big-endian-specific.
	asmSrdi(buffer, TMP, R3, 2);
	asmStw(buffer, TMP, sizeof(Value), R3);
	asmLi(buffer, R0, 0);
	asmStw(buffer, R0, sizeof(Value) + 4, R3);
	asmMr(buffer, R8_PPC, R3);            // r8: cursor base (untagged)

	asmLbz(buffer, R0, isIndexedOffset, R4);
	asmAndiDot(buffer, R0, R0, 1);
	asmBeq(buffer, &notIndexed);
	asmStd(buffer, R5, offsetof(RawIndexedObject, size), R3);
	asmAddi(buffer, R8_PPC, R8_PPC, sizeof(Value));

	asmPpcLabelBind(buffer, &notIndexed, asmOffset(buffer));

	// zero payload (r9 is free from here: the slow path branched earlier)
	asmLbz(buffer, R9_PPC, payloadOffset, R4);   // r9: payload size
	asmCmpdi(buffer, 0, R9_PPC, 0);
	asmStb(buffer, R9_PPC, offsetof(RawObject, payloadSize), R3);
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, R6, R8_PPC, R0);
	asmAddi(buffer, R6, R6, offsetof(RawObject, body)); // r6: inst-vars pointer
	asmBeq(buffer, &noPayload);

	asmPpcLabelBind(buffer, &payloadLoop, asmOffset(buffer));
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, TMP2, R8_PPC, R0);
	asmLi(buffer, R0, 0);
	asmStd(buffer, R0, offsetof(RawObject, body) - sizeof(Value), TMP2);
	asmAddicDot(buffer, R9_PPC, R9_PPC, -1);
	asmBne(buffer, &payloadLoop);

	asmPpcLabelBind(buffer, &noPayload, asmOffset(buffer));

	// nil variables
	asmCmpdi(buffer, 0, R7, 0);
	asmBeq(buffer, &noVars);
	generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
	asmMr(buffer, R9_PPC, R7);
	asmStb(buffer, R7, offsetof(RawObject, varsSize), R3);

	asmPpcLabelBind(buffer, &varsLoop, asmOffset(buffer));
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, TMP2, R6, R0);
	asmStd(buffer, TMP, -(ptrdiff_t) sizeof(Value), TMP2);
	asmAddicDot(buffer, R9_PPC, R9_PPC, -1);
	asmBne(buffer, &varsLoop);

	asmPpcLabelBind(buffer, &noVars, asmOffset(buffer));

	// zero bytes
	asmLbz(buffer, R0, isBytesOffset, R4);
	asmAndiDot(buffer, R0, R0, 1);
	asmBeq(buffer, &noBytes);
	asmCmpdi(buffer, 0, R5, 0);
	asmBeq(buffer, &zeroBytes);
	asmMr(buffer, R9_PPC, R5);
	asmSldi(buffer, R0, R7, 3);
	asmAdd(buffer, R6, R6, R0);           // skip the inst vars

	asmPpcLabelBind(buffer, &zeroBytesLoop, asmOffset(buffer));
	asmAdd(buffer, TMP2, R6, R9_PPC);
	asmLi(buffer, R0, 0);
	asmStb(buffer, R0, -1, TMP2);
	asmAddicDot(buffer, R9_PPC, R9_PPC, -1);
	asmBne(buffer, &zeroBytesLoop);

	asmPpcLabelBind(buffer, &noBytes, asmOffset(buffer));
	asmPpcLabelBind(buffer, &zeroBytes, asmOffset(buffer));

	asmAddi(buffer, R3, R3, 1);           // tag
	asmBlr(buffer);

	asmPpcLabelBind(buffer, &noFreeSpace, asmOffset(buffer));
	// slow path: allocateObject(heap, class, size), r4/r5 already sit in
	// their C argument slots (the same pre-placement trick as SysV x64);
	// r9 still holds the thread base here (the branch precedes its reuse).
	asmLd(buffer, R3, offsetof(Thread, heap), R9_PPC);
	generateCCall(generator, (intptr_t) allocateObject, 3, 0);
	asmAddi(buffer, R3, R3, 1);           // tag
	asmBlr(buffer);
}
StubCode AllocateStub = { .generator = generateAllocate, .id = STUB_ALLOCATE };


static void generateLookup(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	// lookupNativeCode(class, selector): args pre-placed in r3/r4 by
	// generateMethodLookup (the SysV pre-placement trick).
	generateCCall(generator, (intptr_t) lookupNativeCode, 2, 0);
	asmMr(buffer, TGT, R3);
	asmBlr(buffer);
}
StubCode LookupStub = { .generator = generateLookup, .id = STUB_LOOKUP };


static void generateDoesNotUnderstandStub(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel loop;
	AssemblerLabel zeroArgs;
	asmInitLabel(&loop);
	asmInitLabel(&zeroArgs);

	generator->code.methodOrBlock = createDoesNotUnderstandCode();
	generator->frameSize = 3;

	// IN: r3 = selector (tagged), r5 = argsSize, TGT = this trampoline's
	// native code, LR = the original send's return address.
	// Framed-primitive prologue (LR discipline rule 1): push LR first so the
	// frame layout matches x64's call-pushed return address exactly.
	asmMflr(buffer, R0);
	asmPush(buffer, R0);
	asmPush(buffer, FP);
	asmMr(buffer, FP, R1);

	// save native code
	asmPush(buffer, TGT);
	generatePushDummyContext(buffer);

	// spill selector
	asmPush(buffer, R3);

	// allocate arguments array (r5 = the send's argsSize, still live from
	// the trampoline, the same flow as x64's RDX into the stub)
	generateLoadObject(buffer, (RawObject *) Handles.Array->raw, R4, 0);
	generateStubCall(generator, &AllocateStub);

	// fill arguments array from the stack (the send's pushed args sit above
	// the frame: arg i at FP + 24 + i*8)
	asmLdT(buffer, R9_PPC, varOffset(RawArray, size), R3);
	asmCmpdi(buffer, 0, R9_PPC, 0);
	asmBeq(buffer, &zeroArgs);
	asmPpcLabelBind(buffer, &loop, asmOffset(buffer));
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, TMP2, FP, R0);
	asmLd(buffer, TMP, 3 * sizeof(intptr_t), TMP2);
	asmSldi(buffer, R0, R9_PPC, 3);
	asmAdd(buffer, TMP2, R3, R0);
	asmStdT(buffer, TMP, varOffset(RawArray, vars) - sizeof(intptr_t), TMP2);
	asmAddicDot(buffer, R9_PPC, R9_PPC, -1);
	asmBne(buffer, &loop);
	asmPpcLabelBind(buffer, &zeroArgs, asmOffset(buffer));

	// spill arguments array
	asmPush(buffer, R3);
	generator->frameSize++;

	// allocate message
	generateLoadObject(buffer, (RawObject *) Handles.Message->raw, R4, 0);
	asmLi(buffer, R5, 0);
	generateStubCall(generator, &AllocateStub);

	// fill message: the spilled arguments array (popped), then the selector
	asmPop(buffer, R0);
	asmStdT(buffer, R0, varOffset(RawMessage, arguments), R3);
	asmLd(buffer, TMP, -3 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmStdT(buffer, TMP, varOffset(RawMessage, selector), R3);
	asmPush(buffer, R3);

	// lookup #doesNotUnderstand: on the original receiver
	asmLd(buffer, TMP, 2 * sizeof(intptr_t), FP);
	asmPush(buffer, TMP);
	generator->frameSize++;
	generateLoadClass(buffer, TMP, R3);
	generateLoadObject(buffer, (RawObject *) Handles.doesNotUnderstandSymbol->raw, R4, 0);
	generateMethodLookup(generator);

	// call #doesNotUnderstand:
	generator->frameSize -= 2;
	asmCallReg(buffer, TGT);
	generateStackmap(generator);

	// epilogue: drop the 5 frame slots, restore FP and the pushed LR
	asmDropStack(buffer, 5);
	asmPop(buffer, FP);
	asmPop(buffer, R0);
	asmMtlr(buffer, R0);
	asmBlr(buffer);
}
StubCode DoesNotUnderstandStub = { .generator = generateDoesNotUnderstandStub, .id = STUB_DNU };


static CompiledMethod *createDoesNotUnderstandCode(void)
{
	CompiledCodeHeader header = { 0 };
	CompiledMethod *method = handle(allocateObject(CurrentThread.heap, Handles.CompiledMethod->raw, 0));
	SourceCode *source = newObject(Handles.SourceCode, 0);
	sourceCodeSetSourceOrFileName(source, asString("_doesNotUnderstand []"));
	sourceCodeSetPosition(source, 0);
	sourceCodeSetSourceSize(source, 0);
	sourceCodeSetLine(source, 0);
	sourceCodeSetColumn(source, 0);
	compiledMethodSetOwnerClass(method, Handles.UndefinedObject);
	compiledMethodSetSelector(method, getSymbol("_doesNotUnderstand"));
	compiledMethodSetSourceCode(method, source);
	method->raw->header = header;
	return method;
}
