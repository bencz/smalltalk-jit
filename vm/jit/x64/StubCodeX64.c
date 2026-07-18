// x86-64 backend. Compiled only when ST_ARCH=x64 (CMakeLists.txt); the guard
// catches a forced ST_ARCH on the wrong host.
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/StubCode.h"
#include "memory/Heap.h"
#include "core/Lookup.h"
#include "jit/CodeGenerator.h"
#include "jit/CodeDescriptors.h"
#include "jit/x64/AssemblerX64.h"
#include "jit/x64/Abi.h"
#include "core/Thread.h"
#include "core/StackFrame.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"

static CompiledMethod *createDoesNotUnderstandCode(void);


// Stubs are cached per-HEAP (Heap.stubCode[id]), generated once and reused by every
// mutator of that heap. Double-checked locking with acquire/release so the common
// (already-generated) path is lock-free and race-free: the slot is published with a
// release store only after the NativeCode is fully built.
void generateStubCall(CodeGenerator *generator, StubCode *stubCode)
{
	AssemblerBuffer *buffer = &generator->buffer;
	asmMovqImm(buffer, (uint64_t) getStubNativeCode(stubCode)->insts, TMP);
	asmCallq(buffer, TMP);
	generateStackmap(generator);
	if (generator->descriptors != NULL) {
		ordCollAdd(generator->descriptors, createBytecodeDescriptor(asmOffset(buffer), generator->bytecodeNumber));
	}
}


// The C -> Smalltalk entry trampoline. Invoked as a C function pointer, so the
// IN-args arrive in the platform ABI's argument registers (sysv:
// RDI=method, RSI=nativeCode, RDX=args, RCX=thread) and the C callee-saved set
// must be preserved — both come from gX64Abi. Everything else (the
// EntryStackFrame prev/entry/exit protocol, the arg-copy loop, the post-call
// TLS reload for fiber migration) is VM-internal and shared across ABIs.
static void generateSmalltalkEntry(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	const X64Abi *abi = gX64Abi;
	AssemblerLabel loop;
	ptrdiff_t argsOffset = offsetof(RawCompiledMethod, header) + offsetof(CompiledCodeHeader, argsSize);

	Register regMethod = (Register) abi->argRegs[0];
	Register regEntry = (Register) abi->argRegs[1];
	Register regArgs = (Register) abi->argRegs[2];
	Register regThread = (Register) abi->argRegs[3];

	size_t savedRegsSize = (size_t) abi->entrySavedRegsSize;
	size_t frameSize = align(savedRegsSize + sizeof(EntryStackFrame), 16);
	frameSize -= savedRegsSize;

	asmInitLabel(&loop);

	abi->emitEntrySaveRegs(buffer); // lowers RSP by exactly entrySavedRegsSize

	asmMovq(buffer, RSP, RBP);
	asmSubqImm(buffer, RSP, frameSize);

	// load previous entry frame
	asmMovqMem(buffer, asmMem(regThread, NO_REGISTER, SS_1, offsetof(Thread, stackFramesTail)), TMP);
	// setup it in current entry frame
	asmMovqToMem(buffer, TMP, asmMem(RSP, NO_REGISTER, SS_1, offsetof(EntryStackFrame, prev)));
	// setup RBP
	asmMovqToMem(buffer, RBP, asmMem(RSP, NO_REGISTER, SS_1, offsetof(EntryStackFrame, entry)));
	// zero exit frame
	asmXorq(buffer, TMP, TMP);
	asmMovqToMem(buffer, TMP, asmMem(RSP, NO_REGISTER, SS_1, offsetof(EntryStackFrame, exit)));
	// setup entry stack frame to thread
	asmMovqToMem(buffer, RSP, asmMem(regThread, NO_REGISTER, SS_1, offsetof(Thread, stackFramesTail)));

	// load method argumnets size (RBX: VM-internal loop counter, inside the
	// saved area — callee-saved in every supported ABI)
	asmMovzxbMemq(buffer, asmMem(regMethod, NO_REGISTER, SS_1, argsOffset), RBX);
	asmIncq(buffer, RBX); // there is alway 'self' argument
	asmPushq(buffer, RBX);

	// push arguments on stack
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	asmPushqMem(buffer, asmMem(regArgs, RBX, SS_8, -sizeof(intptr_t)));
	asmDecq(buffer, RBX);
	asmJ(buffer, COND_NOT_ZERO, &loop);

	// load context
	asmMovqMem(buffer, asmMem(regThread, NO_REGISTER, SS_1, offsetof(Thread, context)), CTX);

	// invoke method
	asmMovq(buffer, regEntry, R11); // Smalltalk methods expects native code entry in R11
	asmCallq(buffer, regEntry);

	// load the CURRENT worker's thread from TLS (the fiber may have migrated OS threads
	// during the Smalltalk call, so CTX->thread could be stale); regThread doubles
	// as the post-call scratch — it is volatile in every supported ABI
	asmLoadTls(buffer, regThread, gCurrentThreadTpoff);
	// load entry frame
	asmMovqMem(buffer, asmMem(regThread, NO_REGISTER, SS_1, offsetof(Thread, stackFramesTail)), TMP);
	// load previous entry frame
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, offsetof(EntryStackFrame, prev)), TMP);
	// setup entry stack frame to thread
	asmMovqToMem(buffer, TMP, asmMem(regThread, NO_REGISTER, SS_1, offsetof(Thread, stackFramesTail)));

	// restore RSP
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -sizeof(intptr_t) - frameSize), RBX);
	asmLeaq(buffer, asmMem(RSP, RBX, SS_8, sizeof(intptr_t) + frameSize), RSP);

	abi->emitEntryRestoreRegs(buffer);
	asmRet(buffer);
}
StubCode SmalltalkEntry = { .generator = generateSmalltalkEntry, .id = STUB_SMALLTALK_ENTRY };


static void generateAllocate(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel noFreeSpace;
	AssemblerLabel bytes;
	AssemblerLabel align;
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
	asmInitLabel(&align);
	asmInitLabel(&notIndexed);
	asmInitLabel(&noPayload);
	asmInitLabel(&payloadLoop);
	asmInitLabel(&noVars);
	asmInitLabel(&varsLoop);
	asmInitLabel(&noBytes);
	asmInitLabel(&zeroBytes);
	asmInitLabel(&zeroBytesLoop);

	// RSI: class
	// RDX: indexed variables size

	// load instance and variables size
	asmMovzxwMemq(buffer, asmMem(RSI, NO_REGISTER, SS_1, sizeOffset), RCX); // RCX: instance size
	asmMovzxbMemq(buffer, asmMem(RSI, NO_REGISTER, SS_1, varsOffset), RDI); // RDI: instance variables size

	// add indexed fields size
	asmTestbMemImm(buffer, asmMem(RSI, NO_REGISTER, SS_1, isBytesOffset), 1);
	asmJ(buffer, COND_NOT_ZERO, &bytes);

	// pointers shape
	asmLeaq(buffer, asmMem(RCX, RDX, SS_8, 0), RCX); // RCX: instance size
	asmAddq(buffer, RDX, RDI); // RDI: instance variables size
	asmJmpLabel(buffer, &align);

	// bytes shape
	asmLabelBind(buffer, &bytes, asmOffset(buffer));
	asmAddq(buffer, RDX, RCX); // RCX: instance size

	// align
	asmLabelBind(buffer, &align, asmOffset(buffer));
	asmAddqImm(buffer, RCX, HEAP_OBJECT_ALIGN - 1);
	asmAndqImm(buffer, RCX, -HEAP_OBJECT_ALIGN); // RCX: aligned size

	// check free space — bump the RUNNING worker's TLAB, read from TLS (%fs) so shared JIT
	// code allocates into whichever worker is executing this fiber right now (not the worker
	// that created the context, which may differ after a migration)
	asmLoadTls(buffer, RBX, gCurrentThreadTpoff); // RBX: &CurrentThread
	asmMovqMem(buffer, asmMem(RBX, NO_REGISTER, SS_1, tlabOffset + offsetof(TLAB, end)), TMP); // TMP: TLAB end
	asmMovqMem(buffer, asmMem(RBX, NO_REGISTER, SS_1, tlabOffset + offsetof(TLAB, top)), RAX); // RAX: new object
	asmSubq(buffer, RAX, TMP); // TMP: TLAB free space
	asmCmpq(buffer, RCX, TMP);
	asmJ(buffer, COND_ABOVE, &noFreeSpace);

	// move top cursor
	asmAddqToMem(buffer, RCX, asmMem(RBX, NO_REGISTER, SS_1, tlabOffset + offsetof(TLAB, top)));

	// class
	asmMovqToMem(buffer, RSI, asmMem(RAX, NO_REGISTER, SS_1, offsetof(RawObject, class)));
	// header
	asmMovq(buffer, RAX, TMP); // TMP: object hash
	asmShrqImm(buffer, TMP, 2);
	asmMovqImm(buffer, 0xFFFFFFFF, R8); // R8: mask
	asmAndq(buffer, R8, TMP);
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, sizeof(Value)));
	asmMovq(buffer, RAX, R8);

	asmTestbMemImm(buffer, asmMem(RSI, NO_REGISTER, SS_1, isIndexedOffset), 1);
	asmJ(buffer, COND_ZERO, &notIndexed);
	asmMovqToMem(buffer, RDX, asmMem(RAX, NO_REGISTER, SS_1, offsetof(RawIndexedObject, size)));
	asmLeaq(buffer, asmMem(R8, NO_REGISTER, SS_1, sizeof(Value)), R8);

	asmLabelBind(buffer, &notIndexed, asmOffset(buffer));

	// zero payload
	asmMovzxbMemq(buffer, asmMem(RSI, NO_REGISTER, SS_1, payloadOffset), RBX); // RBX: payload size
	asmCmpqImm(buffer, RBX, 0);
	asmMovbToMem(buffer, BL, asmMem(RAX, NO_REGISTER, SS_1, offsetof(RawObject, payloadSize))); // store payload size in instance
	asmLeaq(buffer, asmMem(R8, RBX, SS_8, offsetof(RawObject, body)), RCX); // save pointer to inst vars
	asmJ(buffer, COND_EQUAL, &noPayload);

	asmLabelBind(buffer, &payloadLoop, asmOffset(buffer));
	asmMovqMemImm(buffer, 0, asmMem(R8, RBX, SS_8, offsetof(RawObject, body) - sizeof(Value)));
	asmDecq(buffer, RBX);
	asmJ(buffer, COND_NOT_ZERO, &payloadLoop);

	asmLabelBind(buffer, &noPayload, asmOffset(buffer));

	// nil variables
	asmCmpqImm(buffer, RDI, 0);
	asmJ(buffer, COND_EQUAL, &noVars);
	generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
	asmMovq(buffer, RDI, RBX);
	asmMovbToMem(buffer, BL, asmMem(RAX, NO_REGISTER, SS_1, offsetof(RawObject, varsSize)));

	asmLabelBind(buffer, &varsLoop, asmOffset(buffer));
	asmMovqToMem(buffer, TMP, asmMem(RCX, RBX, SS_8, -sizeof(Value)));
	asmDecq(buffer, RBX);
	asmJ(buffer, COND_NOT_ZERO, &varsLoop);

	asmLabelBind(buffer, &noVars, asmOffset(buffer));

	// zero bytes
	asmTestbMemImm(buffer, asmMem(RSI, NO_REGISTER, SS_1, isBytesOffset), 1);
	asmJ(buffer, COND_ZERO, &noBytes);
	asmCmpqImm(buffer, RDX, 0);
	asmJ(buffer, COND_EQUAL, &zeroBytes);
	asmMovq(buffer, RDX, RBX);
	asmLeaq(buffer, asmMem(RCX, RDI, SS_8, 0), RCX);

	asmLabelBind(buffer, &zeroBytesLoop, asmOffset(buffer));
	asmMovbMemImm(buffer, 0, asmMem(RCX, RBX, SS_1, -1));
	asmDecq(buffer, RBX);
	asmJ(buffer, COND_NOT_ZERO, &zeroBytesLoop);

	asmLabelBind(buffer, &noBytes, asmOffset(buffer));
	asmLabelBind(buffer, &zeroBytes, asmOffset(buffer));

	asmIncq(buffer, RAX);
	asmRet(buffer);

	asmLabelBind(buffer, &noFreeSpace, asmOffset(buffer));
	// heap is now a pointer field: LOAD thread->heap (not thread->heap).
	asmMovqMem(buffer, asmMem(RBX, NO_REGISTER, SS_1, offsetof(Thread, heap)), RDI);
	generateCCall(generator, (intptr_t) allocateObject, 3, 0);
	asmIncq(buffer, RAX);
	asmRet(buffer);
}
StubCode AllocateStub = { .generator = generateAllocate, .id = STUB_ALLOCATE };


static void generateLookup(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	generateCCall(generator, (intptr_t) lookupNativeCode, 2, 0);
	asmMovq(buffer, RAX, R11);
	asmRet(buffer);
}
StubCode LookupStub = { .generator = generateLookup, .id = STUB_LOOKUP };


// The shared PIC probe and dispatch stub, the "shared loop" of the per-site
// inline caches (jit/InlineCache.h): ONE stub per heap, never per-site code.
// In: RDI = TAGGED receiver class, RSI = selector, RDX = cell.
// Out: R11 = entry to call.
// The site's inline guard already missed way 0, so route on the state's kind:
// a pic walks ways[1..size-1] right here; mega runs the plain global probe
// (generateMethodLookup, the pre-IC floor); unlinked, mono-with-another-class
// and an exhausted walk resolve through C (inlineCacheMiss binds/transitions).
// The state is read ONCE into RAX and is immutable; a peer's CAS swings the
// cell to a fresh state and never touches this one, and the STW sweep that
// frees it cannot run mid-walk (no safepoint poll in here).
static void generatePicProbe(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel walk, mega, wayHit, loop;
	asmInitLabel(&walk);
	asmInitLabel(&mega);
	asmInitLabel(&wayHit);
	asmInitLabel(&loop);

	asmMovqMem(buffer, asmMem(RDX, NO_REGISTER, SS_1, offsetof(IcCell, state)), RAX);
	asmMovqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, offsetof(IcState, kind)), TMP);
	asmCmpqImm(buffer, TMP, IC_KIND_PIC);
	asmJ(buffer, COND_EQUAL, &walk);
	asmJ(buffer, COND_GREATER, &mega);                     // IC_KIND_MEGA
	// unlinked or mono-with-another-class: C binds or builds the pic
	generateCCall(generator, (intptr_t) inlineCacheMiss, 3, 0);
	asmMovq(buffer, RAX, R11);
	asmRet(buffer);

	// pic: walk ways[1..size-1] (way 0 is the header the site already tested).
	// RAX = cursor, TMP = end; RDI/RSI/RDX stay intact for the exhausted path.
	asmLabelBind(buffer, &walk, asmOffset(buffer));
	asmMovqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, offsetof(IcState, size)), TMP);
	asmShlqImm(buffer, TMP, 4);                            // size * sizeof(IcWay)
	asmLeaq(buffer, asmMem(RAX, TMP, SS_1, offsetof(IcState, ways)), TMP);
	asmLeaq(buffer, asmMem(RAX, NO_REGISTER, SS_1,
		offsetof(IcState, ways) + sizeof(IcWay)), RAX);
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	asmCmpqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, offsetof(IcWay, class)), RDI);
	asmJ(buffer, COND_EQUAL, &wayHit);
	asmAddqImm(buffer, RAX, sizeof(IcWay));
	asmCmpq(buffer, RAX, TMP);
	asmJ(buffer, COND_BELOW, &loop);
	// exhausted: C extends the pic or promotes to mega
	generateCCall(generator, (intptr_t) inlineCacheMiss, 3, 0);
	asmMovq(buffer, RAX, R11);
	asmRet(buffer);

	asmLabelBind(buffer, &wayHit, asmOffset(buffer));
	if (icStatsEnabled()) {
		asmMovqImm(buffer, (int64_t) &gIcStats.picHits, RDX);
		asmIncqMem(buffer, asmMem(RDX, NO_REGISTER, SS_1, 0));
	}
	asmMovqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, offsetof(IcWay, target)), R11);
	asmRet(buffer);

	// mega (permanent): the class-independent global-cache probe, exactly the
	// pre-IC sequence, shared here instead of bloating every site.
	asmLabelBind(buffer, &mega, asmOffset(buffer));
	if (icStatsEnabled()) {
		asmMovqImm(buffer, (int64_t) &gIcStats.megaProbes, RDX);
		asmIncqMem(buffer, asmMem(RDX, NO_REGISTER, SS_1, 0));
	}
	generateMethodLookup(generator);                       // RDI tagged, RSI selector
	asmRet(buffer);
}
StubCode PicProbeStub = { .generator = generatePicProbe, .id = STUB_PIC_PROBE };


static void generateDoesNotUnderstandStub(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel loop;
	AssemblerLabel zeroArgs;
	asmInitLabel(&loop);
	asmInitLabel(&zeroArgs);

	generator->code.methodOrBlock = createDoesNotUnderstandCode();
	generator->frameSize = 3;

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);

	// save native code
	asmPushq(buffer, R11);
	generatePushDummyContext(buffer);

	// spill selector
	asmPushq(buffer, RDI);

	// allocate arguments array
	generateLoadObject(buffer, (RawObject *) Handles.Array->raw, RSI, 0);
	generateStubCall(generator, &AllocateStub);

	// fill arguments array from stack
	asmMovqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawArray, size)), RBX);
	asmTestq(buffer, RBX, RBX);
	asmJ(buffer, COND_ZERO, &zeroArgs);
	asmLabelBind(buffer, &loop, asmOffset(buffer));
	// args sit above the frame at RBP + 2*word + i*word (receiver is at RBP +
	// 2*word, arg 0 at RBP + 3*word). The loop counts RBX from argsSize down to
	// 1 and fills element RBX-1, so the source of element RBX-1 is arg (RBX-1),
	// i.e. RBP + RBX*word + 2*word. (Was 3*word: an off-by-one that dropped the
	// first argument and appended a trailing garbage slot.)
	asmMovqMem(buffer, asmMem(RBP, RBX, SS_8, 2 * sizeof(intptr_t)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(RAX, RBX, SS_8, varOffset(RawArray, vars) - sizeof(intptr_t)));
	asmDecq(buffer, RBX);
	asmJ(buffer, COND_NOT_ZERO, &loop);
	asmLabelBind(buffer, &zeroArgs, asmOffset(buffer));

	// spill arguments array
	asmPushq(buffer, RAX);
	generator->frameSize++;

	// allocate message
	generateLoadObject(buffer, (RawObject *) Handles.Message->raw, RSI, 0);
	asmXorq(buffer, RDX, RDX);
	generateStubCall(generator, &AllocateStub);

	// fill message
	asmPopqMem(buffer, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawMessage, arguments)));
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -3 * sizeof(intptr_t)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawMessage, selector)));
	asmPushq(buffer, RAX);

	// lookup #doesNotUnderstand:
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), TMP);
	asmPushq(buffer, TMP);
	generator->frameSize++;
	generateLoadClass(buffer, TMP, RDI);
	generateLoadObject(buffer, (RawObject *) Handles.doesNotUnderstandSymbol->raw, RSI, 0);
	generateMethodLookup(generator);

	// call #doesNotUnderstand:
	generator->frameSize -= 2;
	asmCallq(buffer, R11);
	generateStackmap(generator);

	// epilogue
	asmAddqImm(buffer, RSP, 5 * sizeof(intptr_t));
	asmPopq(buffer, RBP);
	asmRet(buffer);
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
