#include "runtime/Primitives.h"
#include "jit/CodeGenerator.h"
#include "jit/InlineCache.h"
#include "core/Object.h"
#include "core/Class.h"
#include "memory/Heap.h"
#include "core/Exception.h"
#include "core/Smalltalk.h"
#include "compiler/Compiler.h"
#include "runtime/Stream.h"
#include "runtime/Socket.h"
#include "os/OsSocket.h"
#include "runtime/Json.h"
#include "runtime/Base64.h"
#include "runtime/BigInt.h"
#include "compiler/Parser.h"
#include "core/Lookup.h"
#include "core/StackFrame.h"
#include "core/Thread.h"
#include "core/Handle.h"
#include "memory/GarbageCollector.h"
#include "core/Entry.h"
#include "os/Os.h"
#include "concurrency/Scheduler.h"
#include "runtime/Message.h"
#include "runtime/Collection.h"
#include "runtime/FileSystem.h"
#include "core/Assert.h"
#include "os/OsThread.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

typedef struct {
	char *name;
	enum { GEN, CCALL } type;
	union {
		void (*generate)(CodeGenerator *);
		PrimitiveResult (*cFunction)();
	};
	uint8_t numArgs;
} Primitive;

static PrimitiveResult arrayEqualsPrimitive(Value receiver, Value operand);
static PrimitiveResult replaceBytesPrimitive(Value self, Value start, Value stop, Value replacement, Value replacementStart);
static PrimitiveResult indexOfBytePrimitive(Value self, Value byte, Value start);
static PrimitiveResult stringAsciiLowercasePrimitive(Value receiver);
static PrimitiveResult stringAsciiUppercasePrimitive(Value receiver);
static PrimitiveResult stringTrimSeparatorsPrimitive(Value receiver);
static PrimitiveResult stringAsciiCaseEqualsPrimitive(Value self, Value other);
static PrimitiveResult stringStartsWithAsciiCasePrimitive(Value self, Value prefix);
static PrimitiveResult stringCopyFromToPrimitive(Value self, Value start, Value stop);
static PrimitiveResult stringToIntegerPrimitive(Value self);
static PrimitiveResult stringSplitByPrimitive(Value self, Value delimiter);
static PrimitiveResult atomicLoadPrimitive(Value self);
static PrimitiveResult atomicStorePrimitive(Value self, Value v);
static PrimitiveResult atomicCompareAndSetPrimitive(Value self, Value expected, Value newValue);
static PrimitiveResult atomicGetAndSetPrimitive(Value self, Value v);
static PrimitiveResult atomicGetAndAddPrimitive(Value self, Value delta);
static PrimitiveResult memoryReleaseFencePrimitive(Value self);
static PrimitiveResult atomicArrayAtPrimitive(Value self, Value index);
static PrimitiveResult atomicArrayAtPutPrimitive(Value self, Value index, Value v);
static PrimitiveResult atomicArrayCompareAndSetPrimitive(Value self, Value index, Value expected, Value newValue);
static PrimitiveResult atomicArrayGetAndSetPrimitive(Value self, Value index, Value v);
static PrimitiveResult atomicArrayGetAndAddPrimitive(Value self, Value index, Value delta);
static PrimitiveResult becomePrimitive(Value object, Value other);
static PrimitiveResult workerParallelPrimitive(Value self, Value blocks);
static PrimitiveResult contextPositionDescriptorPrimitive(Value vContext);
static PrimitiveResult stringAsSymbolPrimitive(Value receiver);
static PrimitiveResult largeIntBinaryPrimitive(Value vSelf, Value vOther, Value vOp, Value vClasses);
static PrimitiveResult largeIntComparePrimitive(Value vSelf, Value vOther, Value vClasses);
static PrimitiveResult largeIntShiftPrimitive(Value vSelf, Value vShift, Value vClasses);
static PrimitiveResult largeIntAsFloatPrimitive(Value vSelf, Value vClasses);
static PrimitiveResult largeIntPrintStringPrimitive(Value vSelf, Value vBase);
static PrimitiveResult largeIntFromStringPrimitive(Value vDigits, Value vBase, Value vNegative, Value vClasses);
static PrimitiveResult floatArrayAtPrimitive(Value vSelf, Value vIndex);
static PrimitiveResult floatArrayAtPutPrimitive(Value vSelf, Value vIndex, Value vValue);
static PrimitiveResult floatArrayReducePrimitive(Value vSelf, Value vOp);
static PrimitiveResult floatArrayDotPrimitive(Value vSelf, Value vOther);
static PrimitiveResult floatArrayBinaryPrimitive(Value vSelf, Value vOther, Value vDest, Value vOp);
static PrimitiveResult floatArrayScalarPrimitive(Value vSelf, Value vScalar, Value vDest, Value vOp);
static PrimitiveResult floatArrayFillPrimitive(Value vSelf, Value vValue);
static PrimitiveResult streamOpenPrimitive(Value fileStream, Value fileName, Value mode);
static PrimitiveResult streamClosePrimitive(Value fileStream, Value descriptor);
static PrimitiveResult streamReadPrimitive(Value vStream, Value descriptor, Value vSize, Value vBuffer, Value vStart);
static PrimitiveResult streamWritePrimitive(Value vStream, Value descriptor, Value vSize, Value vBuffer);
static PrimitiveResult streamFlushPrimitive(Value vStream, Value descriptor);
static PrimitiveResult streamGetPositionPrimitive(Value receiver, Value descriptor);
static PrimitiveResult streamSetPositionPrimitive(Value receiver, Value descriptor, Value position);
static PrimitiveResult streamAvailablePrimitive(Value receiver, Value descriptor);
static PrimitiveResult socketConnectPrimitive(Value socket, Value ip, Value port);
static PrimitiveResult socketBindPrimitive(Value socket, Value ip, Value port, Value queueSize);
static PrimitiveResult socketAcceptPrimitive(Value socket);
static PrimitiveResult jsonParsePrimitive(Value receiver, Value vString);
static PrimitiveResult jsonEncodePrimitive(Value receiver, Value vObject);
static PrimitiveResult socketReadPrimitive(Value self, Value fd, Value buffer, Value size, Value start);
static PrimitiveResult socketWritePrimitive(Value self, Value fd, Value buffer, Value size);
static PrimitiveResult socketSetNoDelayPrimitive(Value self, Value fd);
static PrimitiveResult socketHostLookupPrimitive(Value class, Value vHost);
static PrimitiveResult lastIoErrorPrimitive(Value receiver);
static PrimitiveResult currentMicroTimePrimitive(Value receiver);
static PrimitiveResult initParserPrimitive(Value receiver, Value string);
static PrimitiveResult initStreamParserPrimitive(Value receiver, Value string);
static PrimitiveResult freeParserPrimitive(Value receiver);
static PrimitiveResult parseClassPrimitive(Value receiver);
static PrimitiveResult parseMethodPrimitive(Value receiver);
static PrimitiveResult parseMethodOrBlockPrimitive(Value receiver);
static _Bool initParserFromParserObject(ParserObject *parserObj, Parser *parser);
static void freeParserWithinParserObject(ParserObject *parserObj, Parser *parser);
static ParseError *createParserError(Parser *parser);
static PrimitiveResult parserAtEndPrimitive(Value receiver);
static PrimitiveResult buildClassPrimitive(Value receiver, Value vNode);
static PrimitiveResult compileMethodPrimitive(Value receiver, Value vNode, Value class);
static PrimitiveResult collectGarbagePrimitive(Value receiver);
static PrimitiveResult flushSendCachesPrimitive(Value receiver);
static PrimitiveResult executablePathPrimitive(Value receiver);
static PrimitiveResult classRemoveSelectorPrimitive(Value receiver, Value vSelector);
static PrimitiveResult behaviorLookupSelectorPrimitive(Value receiver, Value vSelector);
static PrimitiveResult buildClassInPrimitive(Value receiver, Value vNode, Value vNs);
static PrimitiveResult compileMethodInPrimitive(Value receiver, Value vNode, Value class, Value vNs);
static PrimitiveResult defaultNamespacePrimitive(Value receiver);
static PrimitiveResult setDefaultNamespacePrimitive(Value receiver, Value vNs);
static PrimitiveResult printHeapPrimitive(Value receiver);
static PrimitiveResult lastGcStatsPrimitive(Value receiver);
static PrimitiveResult processSpawnPrimitive(Value block);
static PrimitiveResult processResumePrimitive(Value self, Value id);
static PrimitiveResult processYieldPrimitive(Value self);
static PrimitiveResult processTerminatePrimitive(Value self, Value id);
static PrimitiveResult processCurrentIdPrimitive(Value self);
static PrimitiveResult processSuspendPrimitive(Value self);
static PrimitiveResult processSleepPrimitive(Value self, Value micros);
static PrimitiveResult monitorEnterPrimitive(Value self);
static PrimitiveResult monitorEnterOnPrimitive(Value self, Value syncObj);
static PrimitiveResult monitorExitPrimitive(Value self);
static PrimitiveResult monitorParkPrimitive(Value self);
static PrimitiveResult floatAddPrimitive(Value self, Value arg);
static PrimitiveResult floatSubPrimitive(Value self, Value arg);
static PrimitiveResult floatMulPrimitive(Value self, Value arg);
static PrimitiveResult floatDivPrimitive(Value self, Value arg);
static PrimitiveResult floatLessThanPrimitive(Value self, Value arg);
static PrimitiveResult floatEqualsPrimitive(Value self, Value arg);
static PrimitiveResult floatTruncatedPrimitive(Value self);
static PrimitiveResult floatFloorPrimitive(Value self);
static PrimitiveResult floatCeilingPrimitive(Value self);
static PrimitiveResult floatRoundedPrimitive(Value self);
static PrimitiveResult floatSqrtPrimitive(Value self);
static PrimitiveResult floatSinPrimitive(Value self);
static PrimitiveResult floatCosPrimitive(Value self);
static PrimitiveResult floatExpPrimitive(Value self);
static PrimitiveResult floatLnPrimitive(Value self);
static PrimitiveResult floatTanPrimitive(Value self);
static PrimitiveResult floatArcSinPrimitive(Value self);
static PrimitiveResult floatArcCosPrimitive(Value self);
static PrimitiveResult floatArcTanPrimitive(Value self);
static PrimitiveResult floatArcTan2Primitive(Value self, Value arg);
static PrimitiveResult floatAsStringPrimitive(Value self);
static PrimitiveResult intAsFloatPrimitive(Value self);
static PrimitiveResult floatExponentPrimitive(Value self);
static PrimitiveResult floatTimesTwoPowerPrimitive(Value self, Value arg);
static PrimitiveResult monotonicNanoTimePrimitive(Value receiver);
static PrimitiveResult localUtcOffsetPrimitive(Value receiver, Value epochSeconds);
static PrimitiveResult randomBytesPrimitive(Value receiver, Value bytes);
static PrimitiveResult getEnvPrimitive(Value receiver, Value name);
static PrimitiveResult commandLinePrimitive(Value receiver);
static PrimitiveResult exitWithCodePrimitive(Value receiver, Value code);
static PrimitiveResult cpuCountPrimitive(Value receiver);
static PrimitiveResult fileStatPrimitive(Value receiver, Value path);
static PrimitiveResult fileMkdirPrimitive(Value receiver, Value path);
static PrimitiveResult fileUnlinkPrimitive(Value receiver, Value path);
static PrimitiveResult fileRmdirPrimitive(Value receiver, Value path);
static PrimitiveResult fileRenamePrimitive(Value receiver, Value from, Value to);
static PrimitiveResult fileListDirPrimitive(Value receiver, Value path);
static PrimitiveResult fileGetCwdPrimitive(Value receiver);
static PrimitiveResult fileChdirPrimitive(Value receiver, Value path);
static PrimitiveResult fileRealpathPrimitive(Value receiver, Value path);
static PrimitiveResult base64EncodePrimitive(Value receiver, Value input);
static PrimitiveResult base64DecodePrimitive(Value receiver, Value input);
static PrimitiveResult base64DecodeBytesPrimitive(Value receiver, Value input);

// The GEN generators and the CCALL trampoline are provided by the CPU backend
// selected at link time (CMake ST_ARCH -> vm/jit/<arch>/Primitives<Arch>.c);
// this header is the arch-neutral contract they fulfil.
#include "jit/TargetPrimitives.h"
#include "jit/CodeDescriptors.h"

static PrimitiveResult contextParentPrimitive(Value vContext)
{
	RawContext *context = (RawContext *) asObject(vContext);
	if (!contextFrameOnCurrentStack(context)) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}

	StackFrame *frame = context->frame;
	RawContext *parent = stackFrameGetParentContext(frame);

	if (parent == NULL) {
		return primSuccess(getTaggedPtr(Handles.nil));
	} else if (parent->class == Handles.MethodContext->raw || parent->class == Handles.BlockContext->raw) {
		return primSuccess(tagPtr(parent));
	} else {
		return primFailed();
	}
}


static PrimitiveResult contextArgumentAt(Value vContext, Value vIndex)
{
	RawContext *context = (RawContext *) asObject(vContext);
	if (!contextFrameOnCurrentStack(context)) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	RawCompiledMethod *code = (RawCompiledMethod *) asObject(context->code);
	intptr_t index = asCInt(vIndex);
	if (index < 0 || index > code->header.argsSize) {
		return primFailed();
	}
	return primSuccess(stackFrameGetArg(context->frame, index));
}


static PrimitiveResult contextTemporaryAt(Value vContext, Value vIndex)
{
	RawContext *context = (RawContext *) asObject(vContext);
	if (!contextFrameOnCurrentStack(context)) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	RawCompiledMethod *code = (RawCompiledMethod *) asObject(context->code);
	intptr_t index = asCInt(vIndex);
	if (index < context->size) {
		return primSuccess(context->vars[index]);
	} else if (context->ic == getTaggedPtr(Handles.nil)) {
		return primSuccess(getTaggedPtr(Handles.nil));
	} else {
		RawStackmap *stackmap = findStackmap(code->nativeCode, (ptrdiff_t) asCInt(context->ic));
		index = index - context->size + FRAME_VARS_OFFSET - 1;
		if (stackmapIncludes(stackmap, index)) {
			return primSuccess(stackFrameGetSlot(context->frame, index));
		} else {
			return primSuccess(getTaggedPtr(Handles.nil));
		}
	}
	return primFailed();
}


// Unhandled-error accounting (Scheduler.c gUnhandledErrors): note is called by
// Exception>>defaultAction right before it terminates the signaling fiber;
// take is the read-and-clear used by VMTools for deliberate probes.
static PrimitiveResult unhandledErrorNotePrimitive(Value receiver)
{
	schedulerNoteUnhandledError();
	return primSuccess(receiver);
}


static PrimitiveResult unhandledErrorTakePrimitive(Value receiver)
{
	return primSuccess(tagInt(schedulerTakeUnhandledErrors()));
}




Primitive Primitives[] = {
	{"AtPrimitive", GEN, generateAtPrimitive},
	{"AtPutPrimitive", GEN, generateAtPutPrimitive},
	{"SizePrimitive", GEN, generateSizePrimitive},
	{"InstVarAtPrimitive", GEN, generateInstVarAtPrimitive},
	{"InstVarAtPutPrimitive", GEN, generateInstVarAtPutPrimitive},
	{"BecomePrimitive", CCALL, .cFunction = becomePrimitive, 2},
	{"IdentityPrimitive", GEN, generateIdentityPrimitive},
	{"HashPrimitive", GEN, generateHashPrimitive},
	{"ClassPrimitive", GEN, generateClassPrimitive},

	{"BehaviorNewPrimitive", GEN, generateBehaviorNewPrimitive},
	{"BehaviorNewSizePrimitive", GEN, generateBehaviorNewSizePrimitive},

	{"CharacterNewPrimitive", GEN, generateCharacterNewPrimitive},
	{"CharacterCodePrimitive", GEN, generateCharacterCodePrimitive},

	{"BlockValuePrimitive", GEN, generateBlockValuePrimitive0Args},
	{"BlockValuePrimitive1", GEN, generateBlockValuePrimitive1Args},
	{"BlockValuePrimitive2", GEN, generateBlockValuePrimitive2Args},
	{"BlockValuePrimitive3", GEN, generateBlockValuePrimitive3Args},
	{"BlockValueArgsPrimitive", GEN, generateBlockValueArgsPrimitive},
	{"BlockWhileTruePrimitive", GEN, generateBlockWhileTrue},
	{"BlockWhileTrue2Primitive", GEN, generateBlockWhileTrue2},
	{"BlockOnExceptionPrimitive", GEN, generateBlockOnExceptionPrimitive},

	{"ContextParentPrimitive", CCALL, .cFunction = contextParentPrimitive, 1},
	{"ContextArgumentAt", CCALL, .cFunction = contextArgumentAt, 2},
	{"ContextTemporaryAt", CCALL, .cFunction = contextTemporaryAt, 2},
	{"ContextPositionDescriptorPrimitive", CCALL, .cFunction = contextPositionDescriptorPrimitive, 1},

	{"ExceptionSignal", GEN, generateExceptionSignalPrimitive},

	{"StringHashPrimitive", GEN, generateStringHashPrimitive},
	{"StringAsSymbolPrimitive", CCALL, .cFunction = stringAsSymbolPrimitive, 1},
	{"ArrayEqualsPrimitive", CCALL, .cFunction = arrayEqualsPrimitive, 2},
	{"ReplaceBytesPrimitive", CCALL, .cFunction = replaceBytesPrimitive, 5},
	{"IndexOfBytePrimitive", CCALL, .cFunction = indexOfBytePrimitive, 3},

	{"IntLessThanPrimitive", GEN, generateIntLessThanPrimitive},
	{"IntAddPrimitive", GEN, generateIntAddPrimitive},
	{"IntSubPrimitive", GEN, generateIntSubPrimitive},
	{"IntMulPrimitive", GEN, generateIntMulPrimitive},
	{"IntQuoPrimitive", GEN, generateIntQuoPrimitive},
	{"IntModPrimitive", GEN, generateIntModPrimitive},
	{"IntRemPrimitive", GEN, generateIntRemPrimitive},
	{"IntNegPrimitive", GEN, generateIntNegPrimitive},
	{"IntAndPrimitive", GEN, generateIntAndPrimitive},
	{"IntOrPrimitive", GEN, generateIntOrPrimitive},
	{"IntXorPrimitive", GEN, generateIntXorPrimitive},
	{"IntShiftPrimitive", GEN, generateIntShiftPrimitive},
	{"IntAsObjectPrimitive", GEN, /*generateIntAsObjectPrimitive*/generateNotImplementedPrimitive},

	{"FloatAddPrimitive", CCALL, .cFunction = floatAddPrimitive, 2},
	{"FloatSubPrimitive", CCALL, .cFunction = floatSubPrimitive, 2},
	{"FloatMulPrimitive", CCALL, .cFunction = floatMulPrimitive, 2},
	{"FloatDivPrimitive", CCALL, .cFunction = floatDivPrimitive, 2},
	{"FloatLessThanPrimitive", CCALL, .cFunction = floatLessThanPrimitive, 2},
	{"FloatEqualsPrimitive", CCALL, .cFunction = floatEqualsPrimitive, 2},
	{"FloatTruncatedPrimitive", CCALL, .cFunction = floatTruncatedPrimitive, 1},
	{"FloatFloorPrimitive", CCALL, .cFunction = floatFloorPrimitive, 1},
	{"FloatCeilingPrimitive", CCALL, .cFunction = floatCeilingPrimitive, 1},
	{"FloatRoundedPrimitive", CCALL, .cFunction = floatRoundedPrimitive, 1},
	{"FloatSqrtPrimitive", CCALL, .cFunction = floatSqrtPrimitive, 1},
	{"FloatSinPrimitive", CCALL, .cFunction = floatSinPrimitive, 1},
	{"FloatCosPrimitive", CCALL, .cFunction = floatCosPrimitive, 1},
	{"FloatExpPrimitive", CCALL, .cFunction = floatExpPrimitive, 1},
	{"FloatLnPrimitive", CCALL, .cFunction = floatLnPrimitive, 1},
	{"FloatTanPrimitive", CCALL, .cFunction = floatTanPrimitive, 1},
	{"FloatArcSinPrimitive", CCALL, .cFunction = floatArcSinPrimitive, 1},
	{"FloatArcCosPrimitive", CCALL, .cFunction = floatArcCosPrimitive, 1},
	{"FloatArcTanPrimitive", CCALL, .cFunction = floatArcTanPrimitive, 1},
	{"FloatArcTan2Primitive", CCALL, .cFunction = floatArcTan2Primitive, 2},
	{"FloatAsStringPrimitive", CCALL, .cFunction = floatAsStringPrimitive, 1},
	{"IntAsFloatPrimitive", CCALL, .cFunction = intAsFloatPrimitive, 1},

	{"StreamOpenPrimitive", CCALL, .cFunction = streamOpenPrimitive, 3},
	{"StreamClosePrimitive", CCALL, .cFunction = streamClosePrimitive, 2},
	{"StreamReadPrimitive", CCALL, .cFunction = streamReadPrimitive, 5},
	{"StreamWritePrimitive", CCALL, .cFunction = streamWritePrimitive, 4},
	{"StreamFlushPrimitive", CCALL, .cFunction = streamFlushPrimitive, 2},
	{"StreamGetPositionPrimitive", CCALL, .cFunction = streamGetPositionPrimitive, 2},
	{"StreamSetPositionPrimitive", CCALL, .cFunction = streamSetPositionPrimitive, 3},
	{"StreamAvailablePrimitive", CCALL, .cFunction = streamAvailablePrimitive, 2},

	{"SocketConnectPrimitive", CCALL, .cFunction = socketConnectPrimitive, 3},
	{"SocketBindPrimitive", CCALL, .cFunction = socketBindPrimitive, 4},
	{"SocketAcceptPrimitive", CCALL, .cFunction = socketAcceptPrimitive, 1},
	{"SocketReadPrimitive", CCALL, .cFunction = socketReadPrimitive, 5},
	{"SocketWritePrimitive", CCALL, .cFunction = socketWritePrimitive, 4},
	{"SocketSetNoDelayPrimitive", CCALL, .cFunction = socketSetNoDelayPrimitive, 2},
	{"SocketHostLookup", CCALL, .cFunction = socketHostLookupPrimitive, 2},

	{"LastIoErrorPrimitive", CCALL, .cFunction = lastIoErrorPrimitive, 1},

	{"JsonParsePrimitive", CCALL, .cFunction = jsonParsePrimitive, 2},
	{"JsonEncodePrimitive", CCALL, .cFunction = jsonEncodePrimitive, 2},

	{"CurrentMicroTimePrimitive", CCALL, .cFunction = currentMicroTimePrimitive, 1},

	{"GCPrimitive", CCALL, .cFunction = collectGarbagePrimitive, 1},
	{"LastGCStatsPrimitive", CCALL, .cFunction = lastGcStatsPrimitive, 1},
	{"PrintHeapPrimitive", CCALL, .cFunction = printHeapPrimitive, 1},
	{"InterruptPrimitive", GEN, generateInterruptPrimitive},
	{"ExitPrimitive", GEN, generateExitPrimitive}, // TODO: remove replace with process primitive

	{"ProcessSpawnPrimitive", CCALL, .cFunction = processSpawnPrimitive, 1},
	{"ProcessResumePrimitive", CCALL, .cFunction = processResumePrimitive, 2},
	{"ProcessYieldPrimitive", CCALL, .cFunction = processYieldPrimitive, 1},
	{"ProcessTerminatePrimitive", CCALL, .cFunction = processTerminatePrimitive, 2},
	{"ProcessCurrentIdPrimitive", CCALL, .cFunction = processCurrentIdPrimitive, 1},
	{"ProcessSuspendPrimitive", CCALL, .cFunction = processSuspendPrimitive, 1},
	{"ProcessSleepPrimitive", CCALL, .cFunction = processSleepPrimitive, 2},
	{"MonitorEnterPrimitive", CCALL, .cFunction = monitorEnterPrimitive, 1},
	{"MonitorExitPrimitive", CCALL, .cFunction = monitorExitPrimitive, 1},
	{"MonitorParkPrimitive", CCALL, .cFunction = monitorParkPrimitive, 1},

	{"WorkerParallelPrimitive", CCALL, .cFunction = workerParallelPrimitive, 2},

	{"ParseClassPrimitive", CCALL, .cFunction = parseClassPrimitive, 1},
	{"ParseMethodPrimitive", CCALL, .cFunction = parseMethodPrimitive, 1},
	{"ParseMethodOrBlockPrimitive", CCALL, .cFunction = parseMethodOrBlockPrimitive, 1},

	{"BuildClassPrimitive", CCALL, .cFunction = buildClassPrimitive, 2},
	{"CompileMethodPrimitive", CCALL, .cFunction = compileMethodPrimitive, 3},
	{"MethodSendPrimitive", GEN, generateMethodSendPrimitive},
	{"MethodSendArgsPrimitive", GEN, generateMethodSendArgsPrimitive},

	// APPEND ONLY: the snapshot bakes primitive indices, never reorder
	{"FloatExponentPrimitive", CCALL, .cFunction = floatExponentPrimitive, 1},
	{"FloatTimesTwoPowerPrimitive", CCALL, .cFunction = floatTimesTwoPowerPrimitive, 2},
	{"BlockUnwindPrimitive", GEN, generateBlockUnwindPrimitive},
	{"StringAsciiLowercasePrimitive", CCALL, .cFunction = stringAsciiLowercasePrimitive, 1},
	{"StringAsciiUppercasePrimitive", CCALL, .cFunction = stringAsciiUppercasePrimitive, 1},
	{"StringTrimSeparatorsPrimitive", CCALL, .cFunction = stringTrimSeparatorsPrimitive, 1},
	{"StringAsciiCaseEqualsPrimitive", CCALL, .cFunction = stringAsciiCaseEqualsPrimitive, 2},
	{"StringStartsWithAsciiCasePrimitive", CCALL, .cFunction = stringStartsWithAsciiCasePrimitive, 2},
	{"StringCopyFromToPrimitive", CCALL, .cFunction = stringCopyFromToPrimitive, 3},
	{"StringToIntegerPrimitive", CCALL, .cFunction = stringToIntegerPrimitive, 1},
	{"StringSplitByPrimitive", CCALL, .cFunction = stringSplitByPrimitive, 2},
	{"AtomicLoadPrimitive", CCALL, .cFunction = atomicLoadPrimitive, 1},
	{"AtomicStorePrimitive", CCALL, .cFunction = atomicStorePrimitive, 2},
	{"AtomicCompareAndSetPrimitive", CCALL, .cFunction = atomicCompareAndSetPrimitive, 3},
	{"AtomicGetAndSetPrimitive", CCALL, .cFunction = atomicGetAndSetPrimitive, 2},
	{"AtomicGetAndAddPrimitive", CCALL, .cFunction = atomicGetAndAddPrimitive, 2},
	{"MemoryReleaseFencePrimitive", CCALL, .cFunction = memoryReleaseFencePrimitive, 1},
	{"AtomicArrayAtPrimitive", CCALL, .cFunction = atomicArrayAtPrimitive, 2},
	{"AtomicArrayAtPutPrimitive", CCALL, .cFunction = atomicArrayAtPutPrimitive, 3},
	{"AtomicArrayCompareAndSetPrimitive", CCALL, .cFunction = atomicArrayCompareAndSetPrimitive, 4},
	{"AtomicArrayGetAndSetPrimitive", CCALL, .cFunction = atomicArrayGetAndSetPrimitive, 3},
	{"AtomicArrayGetAndAddPrimitive", CCALL, .cFunction = atomicArrayGetAndAddPrimitive, 3},
	{"MonitorEnterOnPrimitive", CCALL, .cFunction = monitorEnterOnPrimitive, 2},
	// Appended (NEVER reorder this table): unhandled-error accounting for honest
	// process exit codes (see gUnhandledErrors in Scheduler.c).
	{"UnhandledErrorNotePrimitive", CCALL, .cFunction = unhandledErrorNotePrimitive, 1},
	{"UnhandledErrorTakePrimitive", CCALL, .cFunction = unhandledErrorTakePrimitive, 1},
	// Appended (NEVER reorder this table): OS/system primitives for the stdlib
	// core, monotonic clock, local timezone offset, entropy, environment,
	// command line, exit code, CPU count, and the file-system calls
	// (vm/runtime/FileSystem.c). All generic POSIX, no framework logic.
	{"MonotonicNanoTimePrimitive", CCALL, .cFunction = monotonicNanoTimePrimitive, 1},
	{"LocalUtcOffsetPrimitive", CCALL, .cFunction = localUtcOffsetPrimitive, 2},
	{"RandomBytesPrimitive", CCALL, .cFunction = randomBytesPrimitive, 2},
	{"GetEnvPrimitive", CCALL, .cFunction = getEnvPrimitive, 2},
	{"CommandLinePrimitive", CCALL, .cFunction = commandLinePrimitive, 1},
	{"ExitWithCodePrimitive", CCALL, .cFunction = exitWithCodePrimitive, 2},
	{"CpuCountPrimitive", CCALL, .cFunction = cpuCountPrimitive, 1},
	{"FileStatPrimitive", CCALL, .cFunction = fileStatPrimitive, 2},
	{"FileMkdirPrimitive", CCALL, .cFunction = fileMkdirPrimitive, 2},
	{"FileUnlinkPrimitive", CCALL, .cFunction = fileUnlinkPrimitive, 2},
	{"FileRmdirPrimitive", CCALL, .cFunction = fileRmdirPrimitive, 2},
	{"FileRenamePrimitive", CCALL, .cFunction = fileRenamePrimitive, 3},
	{"FileListDirPrimitive", CCALL, .cFunction = fileListDirPrimitive, 2},
	{"FileGetCwdPrimitive", CCALL, .cFunction = fileGetCwdPrimitive, 1},
	{"FileChdirPrimitive", CCALL, .cFunction = fileChdirPrimitive, 2},
	{"FileRealpathPrimitive", CCALL, .cFunction = fileRealpathPrimitive, 2},
	// Appended (NEVER reorder this table): send-cache invalidation for
	// reflective method-dictionary surgery (jit/InlineCache.h). Class builds,
	// redefinitions and extensions invalidate automatically; this is the
	// manual escape hatch.
	{"FlushSendCachesPrimitive", CCALL, .cFunction = flushSendCachesPrimitive, 1},
	// Appended (NEVER reorder this table): namespace-aware compilation for
	// the package loader (vm/core/Namespace.h) and the session default
	// namespace that eval/parseFile compile against.
	{"BuildClassInPrimitive", CCALL, .cFunction = buildClassInPrimitive, 3},
	{"CompileMethodInPrimitive", CCALL, .cFunction = compileMethodInPrimitive, 4},
	{"DefaultNamespacePrimitive", CCALL, .cFunction = defaultNamespacePrimitive, 1},
	{"SetDefaultNamespacePrimitive", CCALL, .cFunction = setDefaultNamespacePrimitive, 2},
	{"ExecutablePathPrimitive", CCALL, .cFunction = executablePathPrimitive, 1},
	// Appended (NEVER reorder this table): dev-reload surgery behind
	// Behavior>>removeSelector:, invalidating like extend/redefine do.
	{"ClassRemoveSelectorPrimitive", CCALL, .cFunction = classRemoveSelectorPrimitive, 2},
	// Appended (NEVER reorder this table): base-64 fast paths for the core
	// codec (vm/runtime/Base64.c); malformed decode input fails into the .st
	// fallback, which raises the precise InvalidArgumentError.
	{"Base64EncodePrimitive", CCALL, .cFunction = base64EncodePrimitive, 2},
	{"Base64DecodePrimitive", CCALL, .cFunction = base64DecodePrimitive, 2},
	{"Base64DecodeBytesPrimitive", CCALL, .cFunction = base64DecodeBytesPrimitive, 2},
	// Appended (NEVER reorder this table): FloatArray, unboxed IEEE-754 doubles
	// in a BytesShape payload. The accessors matter for correctness; the bulk
	// kernels are the point, since they remove the per-element send, tag decode
	// and boxing that an Array of Floats pays. Every one of them fails into a
	// Smalltalk fallback that raises the precise error.
	{"FloatArrayAtPrimitive", CCALL, .cFunction = floatArrayAtPrimitive, 2},
	{"FloatArrayAtPutPrimitive", CCALL, .cFunction = floatArrayAtPutPrimitive, 3},
	{"FloatArrayReducePrimitive", CCALL, .cFunction = floatArrayReducePrimitive, 2},
	{"FloatArrayDotPrimitive", CCALL, .cFunction = floatArrayDotPrimitive, 2},
	{"FloatArrayBinaryPrimitive", CCALL, .cFunction = floatArrayBinaryPrimitive, 4},
	{"FloatArrayScalarPrimitive", CCALL, .cFunction = floatArrayScalarPrimitive, 4},
	{"FloatArrayFillPrimitive", CCALL, .cFunction = floatArrayFillPrimitive, 2},
	// Appended (NEVER reorder this table): LargeInteger over the magnitude
	// kernels in vm/runtime/BigInt.c. One binary primitive with an opcode
	// rather than eight near-identical slots, since the table is append-only
	// forever. Each fails into a Smalltalk fallback that raises precisely
	// (ZeroDivide for a zero divisor, coercion for a non-Integer operand).
	{"LargeIntBinaryPrimitive", CCALL, .cFunction = largeIntBinaryPrimitive, 4},
	{"LargeIntComparePrimitive", CCALL, .cFunction = largeIntComparePrimitive, 3},
	{"LargeIntShiftPrimitive", CCALL, .cFunction = largeIntShiftPrimitive, 3},
	{"LargeIntAsFloatPrimitive", CCALL, .cFunction = largeIntAsFloatPrimitive, 2},
	{"LargeIntPrintStringPrimitive", CCALL, .cFunction = largeIntPrintStringPrimitive, 2},
	{"LargeIntFromStringPrimitive", CCALL, .cFunction = largeIntFromStringPrimitive, 4},
	// Appended (NEVER reorder this table): the method-dictionary walk behind
	// Behavior>>lookupSelector:, which every perform: and every reflective send
	// pays for. Fails into the Smalltalk walk for a non-Behavior receiver or a
	// non-Symbol selector.
	{"BehaviorLookupSelectorPrimitive", CCALL, .cFunction = behaviorLookupSelectorPrimitive, 2},
};


void registerPrimitives(void)
{
	HandleScope scope;
	openHandleScope(&scope);

	Primitive *p = Primitives;
	Primitive *end = Primitives + sizeof(Primitives) / sizeof(Primitive);

	for (; p < end; p++) {
		setGlobal(p->name, tagInt(p - Primitives + 1));
	}

	closeHandleScope(&scope, NULL);
}


uint16_t primitiveCount(void)
{
	return sizeof(Primitives) / sizeof(Primitive);
}


void generatePrimitive(CodeGenerator *generator, uint16_t primitive)
{
	// Defense in depth: an out-of-range number is rejected at compile time
	// (processPrimitivePragma), so reaching here with one means a corrupt method.
	ASSERT(primitive >= 1 && primitive <= primitiveCount());
	Primitive *prim = Primitives + primitive - 1;
	if (prim->type == GEN) {
		prim->generate(generator);
	} else {
		generateCCallPrimitive(generator, prim->cFunction, prim->numArgs);
	}
}


// Byte-collection equality via memcmp (String/Symbol/ByteArray). Fails for
// non-byte or mixed-class operands so the Smalltalk `=` loop handles them (a
// pointer Array's `=` compares elements by value, not by identity — memcmp
// would be wrong there).
static PrimitiveResult arrayEqualsPrimitive(Value receiver, Value operand)
{
	if (!valueTypeOf(operand, VALUE_POINTER)) {
		return primFailed();
	}
	RawObject *a = asObject(receiver);
	RawObject *b = asObject(operand);
	if (!a->class->instanceShape.isBytes) {
		return primFailed();
	}
	if (a->class != b->class) {
		return primFailed();
	}
	size_t sizeA = rawObjectSize(a);
	if (sizeA != rawObjectSize(b)) {
		return primSuccess(getTaggedPtr(Handles.false));
	}
	_Bool equal = memcmp(getRawObjectIndexedVars(a), getRawObjectIndexedVars(b), sizeA) == 0;
	return primSuccess(getTaggedPtr(equal ? Handles.true : Handles.false));
}


// Bulk memmove for `replaceFrom: start to: stop with: repl startingAt: repStart`
// when both receiver and replacement are byte-shaped (String/Symbol/ByteArray).
// Fails otherwise so the Smalltalk per-element loop (which runs the write barrier
// for pointer Arrays) takes over.
static PrimitiveResult replaceBytesPrimitive(Value vSelf, Value vStart, Value vStop, Value vRepl, Value vRepStart)
{
	if (!valueTypeOf(vRepl, VALUE_POINTER)) {
		return primFailed();
	}
	RawObject *self = asObject(vSelf);
	RawObject *repl = asObject(vRepl);
	if (!self->class->instanceShape.isBytes || !repl->class->instanceShape.isBytes) {
		return primFailed();
	}
	intptr_t start = asCInt(vStart);
	intptr_t stop = asCInt(vStop);
	intptr_t repStart = asCInt(vRepStart);
	intptr_t count = stop - start + 1;
	if (count <= 0) {
		return primSuccess(vSelf);
	}
	intptr_t selfSize = (intptr_t) rawObjectSize(self);
	intptr_t replSize = (intptr_t) rawObjectSize(repl);
	if (start < 1 || stop > selfSize || repStart < 1 || repStart + count - 1 > replSize) {
		return primFailed();
	}
	memmove(getRawObjectIndexedVars(self) + (start - 1),
	        getRawObjectIndexedVars(repl) + (repStart - 1),
	        (size_t) count);
	return primSuccess(vSelf);
}


// memchr for a byte value (0..255) in a byte-shaped collection, starting at a
// 1-based index. Answers the 1-based index or 0.
static PrimitiveResult indexOfBytePrimitive(Value vSelf, Value vByte, Value vStart)
{
	RawObject *self = asObject(vSelf);
	if (!self->class->instanceShape.isBytes) {
		return primFailed();
	}
	intptr_t byte = asCInt(vByte);
	intptr_t start = asCInt(vStart);
	intptr_t size = (intptr_t) rawObjectSize(self);
	if (byte < 0 || byte > 255 || start > size) {
		return primSuccess(tagInt(0));
	}
	if (start < 1) {
		start = 1;
	}
	uint8_t *data = getRawObjectIndexedVars(self);
	uint8_t *found = memchr(data + (start - 1), (int) byte, (size_t) (size - (start - 1)));
	return primSuccess(tagInt(found == NULL ? 0 : (intptr_t) (found - data) + 1));
}


// ---- ASCII string helpers moved to C (the per-char Smalltalk loops were the
// hottest userspace work on the HTTP request path). All match the semantics of
// the Smalltalk fallbacks EXACTLY: Character's fold Table is a pure ASCII fold
// (A-Z<->a-z, identity elsewhere) and isSeparator = {tab,lf,ff,cr,space}. Guarded
// to plain String receivers so Symbol/other byte shapes keep the .st path (which
// preserves the receiver's species). ----

static inline _Bool asciiIsSeparatorByte(uint8_t c)
{
	return c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

// True if two byte runs are equal ignoring ASCII letter case.
static _Bool asciiCaseEqualBytes(const uint8_t *a, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t ca = a[i], cb = b[i];
		if (ca != cb) {
			uint8_t la = ca | 0x20;
			if (!(la >= 'a' && la <= 'z' && la == (cb | 0x20))) {
				return 0;
			}
		}
	}
	return 1;
}

static PrimitiveResult stringAsciiLowercasePrimitive(Value receiver)
{
	RawObject *raw = asObject(receiver);
	if (raw->class != Handles.String->raw) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	String *src = scopeHandle(raw);
	size_t size = src->raw->size;
	String *dst = newString(size);          // may GC; src handle stays valid
	uint8_t *s = (uint8_t *) src->raw->contents;   // re-read AFTER the allocation
	uint8_t *d = (uint8_t *) dst->raw->contents;
	for (size_t i = 0; i < size; i++) {
		uint8_t c = s[i];
		d[i] = (c >= 'A' && c <= 'Z') ? (uint8_t) (c + 32) : c;
	}
	Value result = getTaggedPtr(dst);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}

static PrimitiveResult stringAsciiUppercasePrimitive(Value receiver)
{
	RawObject *raw = asObject(receiver);
	if (raw->class != Handles.String->raw) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	String *src = scopeHandle(raw);
	size_t size = src->raw->size;
	String *dst = newString(size);
	uint8_t *s = (uint8_t *) src->raw->contents;
	uint8_t *d = (uint8_t *) dst->raw->contents;
	for (size_t i = 0; i < size; i++) {
		uint8_t c = s[i];
		d[i] = (c >= 'a' && c <= 'z') ? (uint8_t) (c - 32) : c;
	}
	Value result = getTaggedPtr(dst);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}

static PrimitiveResult stringTrimSeparatorsPrimitive(Value receiver)
{
	RawObject *raw = asObject(receiver);
	if (raw->class != Handles.String->raw) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	String *src = scopeHandle(raw);
	size_t size = src->raw->size;
	uint8_t *s = (uint8_t *) src->raw->contents;
	size_t start = 0, end = size;
	while (start < end && asciiIsSeparatorByte(s[start])) {
		start++;
	}
	while (end > start && asciiIsSeparatorByte(s[end - 1])) {
		end--;
	}
	size_t len = end - start;
	String *dst = newString(len);            // may GC; src handle stays valid
	memcpy(dst->raw->contents, src->raw->contents + start, len);  // re-read src after alloc
	Value result = getTaggedPtr(dst);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}

// ASCII case-insensitive equality without allocating.
static PrimitiveResult stringAsciiCaseEqualsPrimitive(Value vSelf, Value vOther)
{
	if (!valueTypeOf(vOther, VALUE_POINTER)) {
		return primFailed();
	}
	RawObject *a = asObject(vSelf);
	RawObject *b = asObject(vOther);
	if (!a->class->instanceShape.isBytes || !b->class->instanceShape.isBytes) {
		return primFailed();
	}
	size_t na = rawObjectSize(a);
	if (na != rawObjectSize(b)) {
		return primSuccess(getTaggedPtr(Handles.false));
	}
	_Bool equal = asciiCaseEqualBytes(getRawObjectIndexedVars(a), getRawObjectIndexedVars(b), na);
	return primSuccess(getTaggedPtr(equal ? Handles.true : Handles.false));
}

// True if `self` begins with `prefix`, ignoring ASCII letter case. No allocation.
static PrimitiveResult stringStartsWithAsciiCasePrimitive(Value vSelf, Value vPrefix)
{
	if (!valueTypeOf(vPrefix, VALUE_POINTER)) {
		return primFailed();
	}
	RawObject *a = asObject(vSelf);
	RawObject *b = asObject(vPrefix);
	if (!a->class->instanceShape.isBytes || !b->class->instanceShape.isBytes) {
		return primFailed();
	}
	size_t na = rawObjectSize(a), nb = rawObjectSize(b);
	if (nb > na) {
		return primSuccess(getTaggedPtr(Handles.false));
	}
	_Bool match = asciiCaseEqualBytes(getRawObjectIndexedVars(a), getRawObjectIndexedVars(b), nb);
	return primSuccess(getTaggedPtr(match ? Handles.true : Handles.false));
}

// `copyFrom: start to: stop` for plain Strings: allocate + memcpy in one step
// instead of species/new:/replaceFrom: sends. Guarded to Handles.String so
// Symbol/Array/Interval/OrderedCollection keep their .st copyFrom: (species /
// write barrier). Defers to .st for any out-of-range request (same behaviour).
static PrimitiveResult stringCopyFromToPrimitive(Value vSelf, Value vStart, Value vStop)
{
	RawObject *rawSelf = asObject(vSelf);
	if (rawSelf->class != Handles.String->raw) {
		return primFailed();
	}
	intptr_t start = asCInt(vStart);
	intptr_t stop = asCInt(vStop);
	intptr_t size = (intptr_t) rawObjectSize(rawSelf);
	intptr_t newSize = stop - start + 1;
	if (newSize < 0 || start < 1 || stop > size) {
		return primFailed();   // let the .st path decide (empty / error), unchanged
	}
	HandleScope scope;
	openHandleScope(&scope);
	String *src = scopeHandle(rawSelf);
	String *dst = newString((size_t) newSize);        // may GC; src handle stays valid
	if (newSize > 0) {
		memcpy(dst->raw->contents, src->raw->contents + (start - 1), (size_t) newSize);
	}
	Value result = getTaggedPtr(dst);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}

// `asNumber` fast path for a plain base-10 integer that fits SmallInteger.
// Conservative on purpose: parses an optional leading '-' then digits, and FAILS
// (deferring to `Number readFrom:`) on anything else -- empty, a lone sign, '+',
// a '.', an exponent, any trailing non-digit, or an out-of-range magnitude -- so
// float / lenient-trailing / LargeInteger cases keep the exact old behaviour.
static PrimitiveResult stringToIntegerPrimitive(Value vSelf)
{
	RawObject *raw = asObject(vSelf);
	if (!raw->class->instanceShape.isBytes) {
		return primFailed();
	}
	intptr_t size = (intptr_t) rawObjectSize(raw);
	if (size == 0) {
		return primFailed();
	}
	uint8_t *s = getRawObjectIndexedVars(raw);
	intptr_t i = 0;
	int64_t sign = 1;
	if (s[0] == '-') {   // note: readFrom: does NOT accept a leading '+', so we don't either
		sign = -1;
		i = 1;
	}
	if (i >= size) {
		return primFailed();
	}
	const int64_t smiMax = (int64_t) (UINT64_MAX >> 2);   // tagInt payload is 62-bit
	int64_t val = 0;
	for (; i < size; i++) {
		uint8_t c = s[i];
		if (c < '0' || c > '9') {
			return primFailed();
		}
		int d = c - '0';
		if (val > (smiMax - d) / 10) {
			return primFailed();   // overflows SmallInteger -> LargeInteger path in .st
		}
		val = val * 10 + d;
	}
	return primSuccess(tagInt((intptr_t) (sign * val)));
}

// `splitBy: aCharacter` for plain Strings: scan for the delimiter byte and build
// the OrderedCollection of substrings entirely in C. Matches the .st semantics
// exactly: empty segments (adjacent / leading / trailing delimiters) are dropped.
// Uses the shared newOrdColl / ordCollAddObject helpers (same as the JSON parser)
// so the collection is built barrier-safe. Guarded to a plain String receiver and
// a Character delimiter; Symbol/Array/non-char fall through to the .st loop
// (which preserves the receiver's species).
static PrimitiveResult stringSplitByPrimitive(Value vSelf, Value vDelim)
{
	RawObject *rawSelf = asObject(vSelf);
	if (rawSelf->class != Handles.String->raw) {
		return primFailed();
	}
	if (!valueTypeOf(vDelim, VALUE_CHAR)) {
		return primFailed();
	}
	uint8_t delim = (uint8_t) asCChar(vDelim);

	HandleScope scope;
	openHandleScope(&scope);
	String *src = scopeHandle(rawSelf);
	OrderedCollection *result = newOrdColl(8);
	size_t size = src->raw->size;
	size_t last = 0;
	for (size_t i = 0; i < size; i++) {
		if ((uint8_t) src->raw->contents[i] != delim) {
			continue;
		}
		if (i != last) {                       // drop empty segment
			HandleScope seg;
			openHandleScope(&seg);
			size_t len = i - last;
			String *piece = newString(len);    // may GC; src/result handles stay valid
			memcpy(piece->raw->contents, src->raw->contents + last, len);
			ordCollAddObject(result, (Object *) piece);
			closeHandleScope(&seg, NULL);       // piece now rooted by `result`
		}
		last = i + 1;
	}
	if (last < size) {                          // trailing non-empty segment
		HandleScope seg;
		openHandleScope(&seg);
		size_t len = size - last;
		String *piece = newString(len);
		memcpy(piece->raw->contents, src->raw->contents + last, len);
		ordCollAddObject(result, (Object *) piece);
		closeHandleScope(&seg, NULL);
	}
	Value r = getTaggedPtr(result);
	closeHandleScope(&scope, NULL);
	return primSuccess(r);
}


static PrimitiveResult becomePrimitive(Value object, Value other)
{
	HandleScope scope;
	openHandleScope(&scope);
	objectBecome(scopeHandle(asObject(object)), scopeHandle(asObject(other)));
	closeHandleScope(&scope, NULL);
	return primSuccess(other);
}


// ---- Worker parallel: run N blocks on N worker OS threads, one shared heap ----
// Each worker points its Thread at the caller's heap, replicates the well-known
// Handles (they reference shared objects), runs its block, and writes the result
// into a shared results Array. GC across the workers is safepoint-coordinated
// (see allocate()/heapGc* in Heap.c). Best for CPU-bound blocks: the caller's
// OTHER fibers' roots aren't scanned during a worker GC yet, so allocation-heavy
// parallelism is safe only when the caller has no live parked fibers.
typedef struct {
	Object *arrayHandle;   // persistent handle to the blocks array (GC-updated)
	Object *resultsHandle; // persistent handle to the results array
	size_t index;
	SmalltalkHandles handles;
	Heap *heap;
} ParWorkerArg;

static void parallelPrimWorker(void *arg)
{
	ParWorkerArg *w = arg;
	memset(&CurrentThread, 0, sizeof(Thread));
	CurrentThread.heap = w->heap;
	initRememberedSet(&CurrentThread.rememberedSet);
	heapAddMutator(w->heap, &CurrentThread);
	CurrentThread.schedExceptionHandler = &CurrentExceptionHandler; // this worker's own TLS slot
	// Handles are per-heap now (Handle.h): CurrentThread.heap == the shared heap w->heap,
	// whose handles are already populated — no TLS copy needed.
	initThreadContext(&CurrentThread);

	HandleScope scope;
	openHandleScope(&scope);
	RawArray *blocks = (RawArray *) ((Object *) w->arrayHandle)->raw;
	Object *blockH = scopeHandle(asObject(blocks->vars[w->index]));
	EntryArgs args = { .size = 0 };
	// Pass the receiver as a HANDLE (GC-updated), never a raw value: sendMessage
	// generates this worker's per-thread SmalltalkEntry stub on first use (allocating,
	// so a peer-pressured scavenge can move the block) BEFORE it reads args[0]. A raw
	// value would then be stale.
	entryArgsAddObject(&args, blockH);
	Value result = sendMessage(getSymbol("value"), &args);
	if (valueTypeOf(result, VALUE_POINTER)) {
		Object box;
		box.raw = asObject(result);
		arrayAtPutObject((Array *) w->resultsHandle, (ptrdiff_t) w->index, &box);
	} else {
		((RawArray *) ((Object *) w->resultsHandle)->raw)->vars[w->index] = result;
	}
	closeHandleScope(&scope, NULL);

	heapEndMutator(w->heap, &CurrentThread); // leave the mutator set before the thread dies
}

static PrimitiveResult workerParallelPrimitive(Value self, Value blocksArray)
{
	(void) self;
	HandleScope scope;
	openHandleScope(&scope);
	Array *arr = (Array *) scopeHandle(asObject(blocksArray));
	size_t n = ((RawArray *) arr->raw)->size;
	Array *results = newArray(n);
	Object *arrPH = persistHandle(arr);
	Object *resultsPH = persistHandle(results);
	SmalltalkHandles mainHandles = Handles;
	Heap *heap = CurrentThread.heap;

	ParWorkerArg *works = malloc(n * sizeof(ParWorkerArg));
	OsThread *threads = malloc(n * sizeof(OsThread));
	for (size_t i = 0; i < n; i++) {
		works[i].arrayHandle = arrPH;
		works[i].resultsHandle = resultsPH;
		works[i].index = i;
		works[i].handles = mainHandles;
		works[i].heap = heap;
	}
	heapGcEnterBlocked(heap, &CurrentThread); // caller is idle (blocked) during the join
	for (size_t i = 0; i < n; i++) {
		osThreadSpawn(&threads[i], parallelPrimWorker, &works[i]);
	}
	for (size_t i = 0; i < n; i++) {
		osThreadJoin(&threads[i]);
	}
	heapGcLeaveBlocked(heap, &CurrentThread);
	free(works);
	free(threads);

	Value r = getTaggedPtr(resultsPH);
	freeHandle(arrPH);
	freeHandle(resultsPH);
	closeHandleScope(&scope, NULL);
	return primSuccess(r);
}


static PrimitiveResult contextPositionDescriptorPrimitive(Value vContext)
{
	RawContext *context = (RawContext *) asObject(vContext);
	intptr_t ic = isTaggedNil(context->ic) ? 0 : asCInt(context->ic);
	return primSuccess(findSourceCode(asObject(context->code), ic));
}


static PrimitiveResult stringAsSymbolPrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *string = scopeHandle(asObject(receiver));
	Value result = getTaggedPtr(asSymbol(string));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult streamOpenPrimitive(Value receiver, Value fileName, Value mode)
{
	int descriptor = streamOpen((RawString *) asObject(fileName), asCInt(mode));
	return descriptor < 0 ? primFailed() : primSuccess(tagInt(descriptor));
}


static PrimitiveResult streamClosePrimitive(Value stream, Value descriptor)
{
	return streamClose(asCInt(descriptor)) ? primSuccess(stream) : primFailed();
}


static PrimitiveResult streamReadPrimitive(Value vStream, Value descriptor, Value vSize, Value vBuffer, Value vStart)
{
	intptr_t size = asCInt(vSize);
	RawString *buffer = (RawString *) asObject(vBuffer);
	intptr_t start = asCInt(vStart) - 1;

	if (size > buffer->size || start < 0 || start >= buffer->size) {
		return primFailed();
	}

	ptrdiff_t read = streamRead(asCInt(descriptor), buffer->contents + start, size);
	if (read < 0) {
		return primFailed();
	}

	return primSuccess(tagInt(read));
}


static PrimitiveResult streamWritePrimitive(Value vStream, Value descriptor, Value vSize, Value vBuffer)
{
	intptr_t size = asCInt(vSize);
	RawString *buffer = (RawString *) asObject(vBuffer);

	if (size > buffer->size) {
		return primFailed();
	}

	ptrdiff_t written = streamWrite(asCInt(descriptor), buffer->contents, size);
	if (written < 0) {
		return primFailed();
	}

	return primSuccess(tagInt(written));
}


static PrimitiveResult streamFlushPrimitive(Value vStream, Value descriptor)
{
	return streamFlush(asCInt(descriptor)) ? primSuccess(vStream) : primFailed();
}


static PrimitiveResult streamGetPositionPrimitive(Value receiver, Value descriptor)
{
	ptrdiff_t position = streamGetPosition(asCInt(descriptor));
	return position < 0 ? primFailed() : primSuccess(tagInt(position));
}


static PrimitiveResult streamSetPositionPrimitive(Value receiver, Value descriptor, Value position)
{
	return streamSetPosition(asCInt(descriptor), asCInt(position))
		? primSuccess(receiver)
		: primFailed();
}


static PrimitiveResult streamAvailablePrimitive(Value receiver, Value descriptor)
{
	intptr_t available = streamAvailable(asCInt(descriptor));
	return available < 0 ? primFailed() : primSuccess(tagInt(available));
}


// True when vAddr is a heap object big enough to be an InternetAddress whose `address`
// field is a tagged int — i.e. reading addr->address is safe. Without this a wrong-typed
// argument (a String, a bare Object, an InternetAddress whose address ivar is still nil)
// makes asObject/asCInt read out of bounds or assert-abort the whole VM. Fail the
// primitive instead so the Smalltalk fallback raises a catchable IoError.
static _Bool validInternetAddress(Value vAddr)
{
	if (!valueTypeOf(vAddr, VALUE_POINTER)) {
		return 0;
	}
	RawObject *obj = asObject(vAddr);
	if (computeRawObjectSize(obj) < sizeof(RawInternetAddress)) {
		return 0;
	}
	return valueTypeOf(((RawInternetAddress *) obj)->address, VALUE_INT);
}


static PrimitiveResult socketConnectPrimitive(Value socket, Value vAddr, Value port)
{
	if (!validInternetAddress(vAddr) || !valueTypeOf(port, VALUE_INT)) {
		return primFailed();
	}
	RawInternetAddress *addr = (RawInternetAddress *) asObject(vAddr);
	OsFd descriptor = socketConnect(asCInt(addr->address), asCInt(port));
	return descriptor == OS_FD_INVALID ? primFailed() : primSuccess(tagInt(descriptor));
}


static PrimitiveResult socketBindPrimitive(Value socket, Value vAddr, Value port, Value queueSize)
{
	if (!validInternetAddress(vAddr) || !valueTypeOf(port, VALUE_INT) || !valueTypeOf(queueSize, VALUE_INT)) {
		return primFailed();
	}
	RawInternetAddress *addr = (RawInternetAddress *) asObject(vAddr);
	OsFd descriptor = socketBind(asCInt(addr->address), asCInt(port), asCInt(queueSize));
	return descriptor == OS_FD_INVALID ? primFailed() : primSuccess(tagInt(descriptor));
}


static PrimitiveResult socketAcceptPrimitive(Value socket)
{
	RawServerSocket *server = (RawServerSocket *) asObject(socket);
	OsFd descriptor = socketAccept((OsFd) asCInt(server->descriptor));
	return descriptor == OS_FD_INVALID ? primFailed() : primSuccess(tagInt(descriptor));
}


// Explicitly disable Nagle on a descriptor. Connected/accepted sockets already
// get this automatically; this primitive is for explicit control from Smalltalk.
static PrimitiveResult socketSetNoDelayPrimitive(Value self, Value vFd)
{
	osSocketSetNoDelay((OsFd) asCInt(vFd));
	return primSuccess(self);
}


// Read up to `size` bytes into `buffer` at `start` (1-based), parking the
// current fiber until data arrives. Returns the count read (0 == peer closed).
// The buffer is handle-protected because the fiber may be moved by a GC while
// parked, so its raw pointer is re-fetched on every attempt.
static PrimitiveResult socketReadPrimitive(Value self, Value vFd, Value vBuffer, Value vSize, Value vStart)
{
	HandleScope scope;
	openHandleScope(&scope);

	String *buffer = scopeHandle(asObject(vBuffer));
	OsFd fd = (OsFd) asCInt(vFd);
	intptr_t size = asCInt(vSize);
	intptr_t start = asCInt(vStart) - 1;

	if (start < 0 || start >= buffer->raw->size) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	if (size > buffer->raw->size - start) {
		size = buffer->raw->size - start;
	}

	size_t bytesRead = 0;
	_Bool failed = 0;
	for (;;) {
		OsIoStatus status = osSocketRead(fd, buffer->raw->contents + start, (size_t) size, &bytesRead);
		if (status == OS_IO_OK) {
			break;
		}
		if (status == OS_IO_WOULD_BLOCK) {
			schedulerWaitFd(fd, 0);
			continue;
		}
		if (status == OS_IO_INTERRUPTED) {
			continue;
		}
		failed = 1;
		break;
	}

	closeHandleScope(&scope, NULL);
	return failed ? primFailed() : primSuccess(tagInt((intptr_t) bytesRead));
}


// Write exactly `size` bytes from `buffer`, parking the fiber whenever the
// socket send buffer is full. Returns the number of bytes written.
static PrimitiveResult socketWritePrimitive(Value self, Value vFd, Value vBuffer, Value vSize)
{
	HandleScope scope;
	openHandleScope(&scope);

	String *buffer = scopeHandle(asObject(vBuffer));
	OsFd fd = (OsFd) asCInt(vFd);
	intptr_t size = asCInt(vSize);

	if (size > buffer->raw->size) {
		size = buffer->raw->size;
	}

	intptr_t total = 0;
	while (total < size) {
		size_t bytesWritten = 0;
		OsIoStatus status = osSocketWrite(fd, buffer->raw->contents + total,
			(size_t) (size - total), &bytesWritten);
		if (status == OS_IO_OK) {
			total += (intptr_t) bytesWritten;
			continue;
		}
		if (status == OS_IO_WOULD_BLOCK) {
			schedulerWaitFd(fd, 1);
			continue;
		}
		if (status == OS_IO_INTERRUPTED) {
			continue;
		}
		closeHandleScope(&scope, NULL);
		return primFailed();
	}

	closeHandleScope(&scope, NULL);
	return primSuccess(tagInt(total));
}


static PrimitiveResult socketHostLookupPrimitive(Value class, Value vHost)
{
	HandleScope scope;
	openHandleScope(&scope);

	InternetAddress *addr = newObject(scopeHandle(asObject(class)), 0);
	String *host = (String *) scopeHandle(asObject(vHost));

	char space[256];
	char *buffer = space;
	if (host->raw->size > 256) {
		String *tmpString = (String *) copyResizedObject((Object *) host, host->raw->size + 1);
		buffer = tmpString->raw->contents;
		buffer[host->raw->size] = '\0';
	} else {
		stringPrintOn(host, buffer);
	}

	const char *error;
	addr->raw->address = tagInt(socketHostLookup(buffer, &error));

	Value result = getTaggedPtr(addr);
	closeHandleScope(&scope, NULL);
	return error == NULL ? primSuccess(result) : primFailed();
}


static PrimitiveResult lastIoErrorPrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);
	Value error = getTaggedPtr(getLastIoError());
	closeHandleScope(&scope, NULL);
	return primSuccess(error);
}


// Json class >> parse: — the strict fast-path parser (vm/Json.c). Fails on any
// syntax error, over-SmallInteger integer or over-depth input; the Smalltalk
// fallback (Json.st slowParse:) then re-parses for a precise JsonParseError or
// a LargeInteger. The result Value is captured BEFORE closing the scope and
// nothing allocates after (the floatResult lesson: never return a handle
// through closeHandleScope, it would leak one caller-scope slot per call).
// Json class >> encode: / primEncode: — the core-type fast path (vm/Json.c).
// Fails on any non-core class in the graph, NaN/Infinity or over-depth; the
// Smalltalk fallback then walks that level reflectively (re-entering this
// primitive for core subtrees).
static PrimitiveResult jsonEncodePrimitive(Value receiver, Value vObject)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *encoded;
	if (!jsonEncode(vObject, &encoded)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Value result = getTaggedPtr(encoded);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult jsonParsePrimitive(Value receiver, Value vString)
{
	if (!valueTypeOf(vString, VALUE_POINTER)) {
		return primFailed();
	}
	RawClass *class = asObject(vString)->class;
	if (class != Handles.String->raw && class != Handles.Symbol->raw) {
		return primFailed();
	}

	HandleScope scope;
	openHandleScope(&scope);
	String *input = (String *) scopeHandle(asObject(vString));
	Value result;
	if (!jsonParse(input, &result)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


// Base64 class >> encode: — byte fast path (vm/runtime/Base64.c). Exact
// String/ByteArray inputs only; anything else fails into the .st fallback,
// which raises the precise InvalidArgumentError.
static PrimitiveResult base64EncodePrimitive(Value receiver, Value vInput)
{
	if (!valueTypeOf(vInput, VALUE_POINTER)) {
		return primFailed();
	}
	RawClass *class = asObject(vInput)->class;
	if (class != Handles.String->raw && class != Handles.ByteArray->raw) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	Object *input = scopeHandle(asObject(vInput));
	String *encoded;
	if (!base64Encode(input, &encoded)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Value result = getTaggedPtr(encoded);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


// Base64 class >> decode: / decodeBytes: — strict decode; malformed input
// fails BEFORE any allocation and the .st fallback re-scans for its error.
static PrimitiveResult base64DecodeToClass(Value vInput, Class *outputClass)
{
	if (!valueTypeOf(vInput, VALUE_POINTER)) {
		return primFailed();
	}
	if (asObject(vInput)->class != Handles.String->raw) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	String *input = (String *) scopeHandle(asObject(vInput));
	Object *decoded;
	if (!base64Decode(input, outputClass, &decoded)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Value result = getTaggedPtr(decoded);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult base64DecodePrimitive(Value receiver, Value vInput)
{
	return base64DecodeToClass(vInput, Handles.String);
}


static PrimitiveResult base64DecodeBytesPrimitive(Value receiver, Value vInput)
{
	return base64DecodeToClass(vInput, Handles.ByteArray);
}


static PrimitiveResult currentMicroTimePrimitive(Value receiver)
{
	return primSuccess(tagInt(osCurrentMicroTime()));
}


// ---- process / scheduling primitives -------------------------------------

// Create a fiber that will evaluate `block` (suspended). Returns its id.
static PrimitiveResult processSpawnPrimitive(Value block)
{
	return primSuccess(tagInt((intptr_t) schedulerSpawnBlock(block)));
}


// Schedule a suspended fiber by id.
static PrimitiveResult processResumePrimitive(Value self, Value id)
{
	schedulerResume((size_t) asCInt(id));
	return primSuccess(self);
}


// Yield the CPU cooperatively; returns when scheduled again.
static PrimitiveResult processYieldPrimitive(Value self)
{
	schedulerYield();
	return primSuccess(self);
}


// Terminate a fiber by id. Does not return if it is the current fiber.
static PrimitiveResult processTerminatePrimitive(Value self, Value id)
{
	schedulerTerminate((size_t) asCInt(id));
	return primSuccess(self);
}


// Id of the currently running fiber.
static PrimitiveResult processCurrentIdPrimitive(Value self)
{
	return primSuccess(tagInt((intptr_t) schedulerCurrentId()));
}


// Park the current fiber until it is explicitly resumed (semaphore/channel).
static PrimitiveResult processSuspendPrimitive(Value self)
{
	schedulerSuspend();
	return primSuccess(self);
}


// Park the current fiber for at least `micros` microseconds.
static PrimitiveResult processSleepPrimitive(Value self, Value micros)
{
	schedulerSleep((int64_t) asCInt(micros));
	return primSuccess(self);
}


// ---- Sync monitor primitives (Semaphore/Channel/... thread-safety), striped ----
// Each sync object's logical monitor is backed by one of heap->monitorLocks[N], chosen
// by mixing the object's GC-stable identity hash. The stripe is computed ONCE at
// monitorEnterOn: and stashed on the thread; monitorExit/monitorPark read the stash and
// never recompute, so enter/exit/park provably drop the same lock they took even across a
// GC move or a become:. The Fibonacci mix (Knuth's 2654435761) decorrelates the
// low-entropy, address-derived hash so stripes are used evenly; N<=1 forces stripe 0
// (== the legacy single global monitor).
static inline size_t stripeForSyncObject(Heap *heap, Value syncObj)
{
	if (heap->monitorStripeCount <= 1 || !valueTypeOf(syncObj, VALUE_POINTER)) {
		return 0;
	}
	uint32_t h = asObject(syncObj)->hash;
	return (size_t) ((uint32_t) (h * 2654435761u) >> heap->monitorStripeShift);
}

// Acquire the stripe for `syncObj` and stash it so the matching exit/park drop exactly
// this lock (GC-safe: waiting counts as at-safepoint). Reentrancy is a bug — the sync
// critical sections must stay FLAT (one stripe at a time) — so assert not-already-held.
static PrimitiveResult monitorEnterOnPrimitive(Value self, Value syncObj)
{
	ASSERT(!CurrentThread.heldMonitor);
	size_t stripe = stripeForSyncObject(CurrentThread.heap, syncObj);
	heapMonitorEnterStripe(CurrentThread.heap, stripe);
	CurrentThread.heldMonitorStripe = stripe;
	CurrentThread.heldMonitor = 1;
	return primSuccess(self);
}

// Acquire stripe 0 (legacy global monitor / any object-less use). Stash stripe 0.
static PrimitiveResult monitorEnterPrimitive(Value self)
{
	ASSERT(!CurrentThread.heldMonitor);
	heapMonitorEnterStripe(CurrentThread.heap, 0);
	CurrentThread.heldMonitorStripe = 0;
	CurrentThread.heldMonitor = 1;
	return primSuccess(self);
}


// Release the stripe this thread holds (stashed at enter).
static PrimitiveResult monitorExitPrimitive(Value self)
{
	size_t stripe = CurrentThread.heldMonitorStripe;
	CurrentThread.heldMonitor = 0;
	heapMonitorExitStripe(CurrentThread.heap, stripe);
	return primSuccess(self);
}


// Park the current fiber, atomically dropping the held stripe (lost-wakeup-safe). The
// caller must hold the stripe; it does NOT re-acquire it on wake.
static PrimitiveResult monitorParkPrimitive(Value self)
{
	size_t stripe = CurrentThread.heldMonitorStripe;
	CurrentThread.heldMonitor = 0;
	schedulerParkAndUnlockMonitorStripe(stripe);
	return primSuccess(self);
}


// ---- Generic atomics (Atomic / AtomicInteger) --------------------------------
// A one-slot cell whose `value` ivar (slot 0) is mutated with machine atomics
// instead of the global sync monitor. This is a general runtime facility (id
// counters, flags, seqlocks, lock-free structures); the actor system is just one
// client. Memory orders mirror the tree's palette (acquire load / release store /
// relaxed) and add ACQ_REL for the read-modify-write ops (exchange/fetch-add and
// CAS-success), which both consume the old value and publish the new one — correct
// on x86-TSO and on weakly-ordered POWER alike. GC is stop-the-world at safepoints,
// so these mutator ops never race a moving collector.

// Address of the cell's single Value slot. asObject strips only the pointer tag; a
// young object's slot stays at (base|SPACE_TAG)+16, i.e. 8-mod-16 → 8-byte aligned,
// so 64-bit __atomic DOUBLEWORD ops are correctly aligned (do NOT widen to 16 bytes:
// young slots are only 8-mod-16).
static inline Value *atomicCellSlot(Value self)
{
	return &getRawObjectVars(asObject(self))[0];
}

// Generational write barrier for an atomic POINTER publish: if an OLD cell now holds
// a YOUNG referent, remember the cell. The TAG_REMEMBERED claim is an atomic OR so
// concurrent publishers to the same shared cell remember it exactly once; the winner
// appends to its OWN per-thread remembered set (per-thread cursor → no cross-thread
// race). Immediates never need a barrier. Runs only between safepoints (STW GC).
static inline void atomicRememberIfNeeded(RawObject *cell, Value stored)
{
	if (!valueTypeOf(stored, VALUE_POINTER)) {
		return;
	}
	RawObject *referent = asObject(stored);
	if (!isOldObject(cell) || !isNewObject(referent)) {
		return;
	}
	uint8_t prev = __atomic_fetch_or(&cell->tags, TAG_REMEMBERED, __ATOMIC_RELAXED);
	if ((prev & TAG_REMEMBERED) != 0) {
		return; // another publisher already remembered this cell
	}
	// Append to this thread's remembered set. Mirrors rememberedSetAdd's body minus
	// the tag set we just did atomically (and its assert, which a racing publisher
	// would otherwise trip).
	RememberedSet *rememberedSet = &CurrentThread.rememberedSet;
	RememberedSetBlock *block = rememberedSet->blocks;
	if (block->current >= block->end) {
		rememberedSetGrow(rememberedSet);
		block = rememberedSet->blocks;
	}
	*block->current++ = tagPtr(cell);
}

// Atomic acquire load of the cell's value.
static PrimitiveResult atomicLoadPrimitive(Value self)
{
	return primSuccess(__atomic_load_n(atomicCellSlot(self), __ATOMIC_ACQUIRE));
}

// Atomic release store of `v` into the cell (+ write barrier on a pointer publish).
static PrimitiveResult atomicStorePrimitive(Value self, Value v)
{
	RawObject *cell = asObject(self);
	__atomic_store_n(&getRawObjectVars(cell)[0], v, __ATOMIC_RELEASE);
	atomicRememberIfNeeded(cell, v);
	return primSuccess(self);
}

// Atomic compare-and-set: if the cell holds `expected`, replace it with `newValue`.
// Answers true/false; barrier on a successful pointer publish. CAS-failure order is
// ACQUIRE (≤ the ACQ_REL success order) but the failed value is not returned — a
// retry loop re-reads through `get` (acquire).
static PrimitiveResult atomicCompareAndSetPrimitive(Value self, Value expected, Value newValue)
{
	RawObject *cell = asObject(self);
	Value *slot = &getRawObjectVars(cell)[0];
	_Bool ok = __atomic_compare_exchange_n(slot, &expected, newValue, 0,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (ok) {
		atomicRememberIfNeeded(cell, newValue);
	}
	return primSuccess(getTaggedPtr(ok ? Handles.true : Handles.false));
}

// Atomic exchange: store `v`, answer the previous value. Barrier on a pointer publish.
static PrimitiveResult atomicGetAndSetPrimitive(Value self, Value v)
{
	RawObject *cell = asObject(self);
	Value old = __atomic_exchange_n(&getRawObjectVars(cell)[0], v, __ATOMIC_ACQ_REL);
	atomicRememberIfNeeded(cell, v);
	return primSuccess(old);
}

// Atomic fetch-add for AtomicInteger. The delta arrives ALREADY tagged (k<<2) and the
// slot holds a tagged SmallInt (i<<2); adding the raw words yields (i+k)<<2 with the
// tag bits (00) preserved, a valid tagged SmallInt, for positive or negative deltas.
// Answers the previous (tagged) value. Fails for a non-integer delta so the Smalltalk
// fallback reports the misuse. No barrier (immediates only). 62-bit range: a raw add
// does not promote to LargeInteger on overflow.
static PrimitiveResult atomicGetAndAddPrimitive(Value self, Value delta)
{
	if (!valueTypeOf(delta, VALUE_INT)) {
		return primFailed();
	}
	Value old = __atomic_fetch_add(atomicCellSlot(self), delta, __ATOMIC_ACQ_REL);
	return primSuccess(old);
}

// Release memory fence: prior mutator stores are ordered before any store issued
// after the fence, so a lock-free publisher can build a structure, fence, then
// publish a pointer with a plain (JIT-emitted) store — a reader that observes the
// pointer via a bare load + address-dependent loads sees the fully-built structure
// with NO reader-side fence. Free on x86-TSO (compiles to a compiler barrier only);
// an lwsync on weakly-ordered POWER. Governs JIT-emitted mutator memory ops, so the
// hardware model (not the C abstract machine) is what matters: lwsync + dependent
// load is the standard hardware publication pattern.
static PrimitiveResult memoryReleaseFencePrimitive(Value self)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
	return primSuccess(self);
}

// ---- AtomicArray: per-index atomics on a wrapped Array ------------------------
// An AtomicArray wraps a plain Array in its first slot; these primitives atomically
// operate on element `index` (1-based) of that Array, reusing the same memory orders
// and write barrier as the scalar Atomic. The Array's element slots (RawArray.vars,
// after the header + size word) are 8-byte aligned, so 64-bit __atomic doubleword
// ops are correctly aligned. Answers NULL (→ primFailed, Smalltalk raises) for a
// non-integer or out-of-range index; `*arrOut` is the backing Array (the barrier
// container). GC is STW at safepoints, so these never race a moving collector.
static inline Value *atomicArraySlot(Value self, Value index, RawObject **arrOut)
{
	if (!valueTypeOf(index, VALUE_INT)) {
		return NULL;
	}
	RawObject *arr = asObject(getRawObjectVars(asObject(self))[0]);
	intptr_t idx = asCInt(index) - 1; // Smalltalk indices are 1-based
	if (idx < 0 || (size_t) idx >= rawObjectSize(arr)) {
		return NULL;
	}
	*arrOut = arr;
	return &((RawArray *) arr)->vars[idx];
}

static PrimitiveResult atomicArrayAtPrimitive(Value self, Value index)
{
	RawObject *arr;
	Value *slot = atomicArraySlot(self, index, &arr);
	if (slot == NULL) {
		return primFailed();
	}
	return primSuccess(__atomic_load_n(slot, __ATOMIC_ACQUIRE));
}

static PrimitiveResult atomicArrayAtPutPrimitive(Value self, Value index, Value v)
{
	RawObject *arr;
	Value *slot = atomicArraySlot(self, index, &arr);
	if (slot == NULL) {
		return primFailed();
	}
	__atomic_store_n(slot, v, __ATOMIC_RELEASE);
	atomicRememberIfNeeded(arr, v);
	return primSuccess(v);
}

static PrimitiveResult atomicArrayCompareAndSetPrimitive(Value self, Value index, Value expected, Value newValue)
{
	RawObject *arr;
	Value *slot = atomicArraySlot(self, index, &arr);
	if (slot == NULL) {
		return primFailed();
	}
	_Bool ok = __atomic_compare_exchange_n(slot, &expected, newValue, 0,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (ok) {
		atomicRememberIfNeeded(arr, newValue);
	}
	return primSuccess(getTaggedPtr(ok ? Handles.true : Handles.false));
}

static PrimitiveResult atomicArrayGetAndSetPrimitive(Value self, Value index, Value v)
{
	RawObject *arr;
	Value *slot = atomicArraySlot(self, index, &arr);
	if (slot == NULL) {
		return primFailed();
	}
	Value old = __atomic_exchange_n(slot, v, __ATOMIC_ACQ_REL);
	atomicRememberIfNeeded(arr, v);
	return primSuccess(old);
}

static PrimitiveResult atomicArrayGetAndAddPrimitive(Value self, Value index, Value delta)
{
	if (!valueTypeOf(delta, VALUE_INT)) {
		return primFailed();
	}
	RawObject *arr;
	Value *slot = atomicArraySlot(self, index, &arr);
	if (slot == NULL) {
		return primFailed();
	}
	return primSuccess(__atomic_fetch_add(slot, delta, __ATOMIC_ACQ_REL));
}


static PrimitiveResult parseClassPrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);

	ParserObject *parserObj = scopeHandle(asObject(receiver));
	Parser parser;
	if (!initParserFromParserObject(parserObj, &parser)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}

	ClassNode *node = parseClass(&parser);
	Value result = getTaggedPtr(node == NULL ? (Object *) createParserError(&parser) : (Object *) node);

	objectStorePtr((Object *) parserObj, &parserObj->raw->atEnd, asBool(parserAtEnd(&parser)));
	freeParserWithinParserObject(parserObj, &parser);

	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult parseMethodPrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);

	ParserObject *parserObj = scopeHandle(asObject(receiver));
	Parser parser;
	if (!initParserFromParserObject(parserObj, &parser)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}

	MethodNode *node = parseMethod(&parser);
	Value result = getTaggedPtr(node == NULL ? (Object *) createParserError(&parser) : (Object *) node);

	objectStorePtr((Object *) parserObj, &parserObj->raw->atEnd, asBool(parserAtEnd(&parser)));
	freeParser(&parser);

	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult parseMethodOrBlockPrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);

	ParserObject *parserObj = scopeHandle(asObject(receiver));
	Parser parser;
	if (!initParserFromParserObject(parserObj, &parser)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}

	Object *node;
	if (currentToken(&parser.tokenizer)->type == TOKEN_OPEN_SQUARE_BRACKET) {
		node = (Object *) parseBlock(&parser);
	} else {
		node = (Object *) parseMethod(&parser);
	}
	Value result = getTaggedPtr(node == NULL ? (Object *) createParserError(&parser) : node);

	objectStorePtr((Object *) parserObj, &parserObj->raw->atEnd, asBool(parserAtEnd(&parser)));
	freeParser(&parser);

	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static _Bool initParserFromParserObject(ParserObject *parserObj, Parser *parser)
{
	String *source = scopeHandle(asObject(parserObj->raw->source));
	if (!isTaggedNil(parserObj->raw->stream)) {
		FileStream *stream = scopeHandle(asObject(parserObj->raw->stream));
		FILE *file = fdopen(dup(asCInt(stream->raw->descriptor)), "r");
		if (file == NULL) {
			return 0;
		}
		initFileParser(parser, file, source);
	} else {
		initParser(parser, source);
		// TODO: preserve position in source string or use CollectionStream?
	}
	return 1;
}


static void freeParserWithinParserObject(ParserObject *parserObj, Parser *parser)
{
	String *source = scopeHandle(asObject(parserObj->raw->source));
	if (!isTaggedNil(parserObj->raw->stream)) {
		ASSERT(parser->tokenizer.isFile);
		FileStream *stream = scopeHandle(asObject(parserObj->raw->stream));
		FILE *file = parser->tokenizer.source.file;
		size_t restart = currentToken(&parser->tokenizer)->position - 1;
		// fclose FIRST: the FILE* wraps a dup of the stream's descriptor, so
		// they SHARE one file offset, and closing a read FILE* seeks that
		// offset back to its own buffered logical position. Restoring before
		// the fclose let that sync silently clobber the restart position,
		// which broke the second parseClass of any multi-class file.
		fclose(file);
		streamSetPosition(asCInt(stream->raw->descriptor), restart);
	}
	freeParser(parser);
}


static ParseError *createParserError(Parser *parser)
{
	Token *token = currentToken(&parser->tokenizer);
	ParseError *error = newObject(Handles.ParseError, 0);
	objectStorePtr((Object *) error, &error->raw->token, (Object *) asString(token->content));
	objectStorePtr((Object *) error, &error->raw->sourceCode, (Object *) createSourceCode(parser, 1));
	return error;
}


static PrimitiveResult buildClassPrimitive(Value receiver, Value vNode)
{
	HandleScope scope;
	openHandleScope(&scope);

	ClassNode *node = scopeHandle(asObject(vNode));
	if (node->raw->class != Handles.ClassNode->raw) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Object *class = buildClass(node);
	Value result = getTaggedPtr(class);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult buildClassInPrimitive(Value receiver, Value vNode, Value vNs)
{
	HandleScope scope;
	openHandleScope(&scope);

	ClassNode *node = scopeHandle(asObject(vNode));
	Namespace *ns = scopeHandle(asObject(vNs));
	if (node->raw->class != Handles.ClassNode->raw
			|| ns->raw->class != Handles.Namespace->raw) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Object *class = buildClassIn(node, ns);
	Value result = getTaggedPtr(class);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult compileMethodInPrimitive(Value receiver, Value vNode, Value class, Value vNs)
{
	HandleScope scope;
	openHandleScope(&scope);

	MethodNode *node = scopeHandle(asObject(vNode));
	Namespace *ns = scopeHandle(asObject(vNs));
	if (node->raw->class != Handles.MethodNode->raw
			|| ns->raw->class != Handles.Namespace->raw) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Value result = getTaggedPtr(compileMethodIn(node, scopeHandle(asObject(class)), ns));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult defaultNamespacePrimitive(Value receiver)
{
	return primSuccess(Handles.DefaultNamespace->raw->value);
}


static PrimitiveResult setDefaultNamespacePrimitive(Value receiver, Value vNs)
{
	HandleScope scope;
	openHandleScope(&scope);

	Namespace *ns = scopeHandle(asObject(vNs));
	if (ns->raw->class != Handles.Namespace->raw) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	objectStorePtr((Object *) Handles.DefaultNamespace,
		&Handles.DefaultNamespace->raw->value, (Object *) ns);
	closeHandleScope(&scope, NULL);
	return primSuccess(receiver);
}


static PrimitiveResult compileMethodPrimitive(Value receiver, Value vNode, Value class)
{
	HandleScope scope;
	openHandleScope(&scope);

	MethodNode *node = scopeHandle(asObject(vNode));
	if (node->raw->class != Handles.MethodNode->raw) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Value result = getTaggedPtr(compileMethod(node, scopeHandle(asObject(class))));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult collectGarbagePrimitive(Value receiver)
{
	collectGarbage(&CurrentThread);
	return primSuccess(receiver);
}


static PrimitiveResult flushSendCachesPrimitive(Value receiver)
{
	icInvalidateAllSends(NULL, NULL);
	return primSuccess(receiver);
}


static PrimitiveResult classRemoveSelectorPrimitive(Value receiver, Value vSelector)
{
	HandleScope scope;
	openHandleScope(&scope);

	// Tag-safe checks FIRST, on the tagged Values: the selector is an ordinary
	// argument, so `removeSelector: 42` arrives as an IMMEDIATE and the old
	// asObject-then-inspect order aborted the VM on it (assertion in a debug
	// build, wild pointer in a release one) instead of failing into the
	// Smalltalk body that raises properly. receiver must be a Class (its class
	// is a metaclass) or a MetaClass.
	RawClass *receiverClass = getClassOf(receiver);
	RawClass *selectorClass = getClassOf(vSelector);
	_Bool isClass = receiverClass != NULL
		&& receiverClass->class == Handles.MetaClass->raw;
	_Bool isMetaClass = receiverClass == Handles.MetaClass->raw;
	if ((!isClass && !isMetaClass)
			|| (selectorClass != Handles.Symbol->raw
				&& selectorClass != Handles.String->raw)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	Object *holder = scopeHandle(asObject(receiver));
	String *selector = scopeHandle(asObject(vSelector));
	if (!classRemoveSelector(holder, selector)) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	closeHandleScope(&scope, NULL);
	return primSuccess(receiver);
}


// Behavior>>lookupSelector: as the C walk it already is internally. The
// Smalltalk version (packages/Core/src/Behavior.st) walks the superclass chain
// with a dictionary send and a block activation PER LEVEL: measured at ~370ns a
// call on this machine, and it sits under every perform: and every reflective
// send in the image.
//
// Deliberately NOT routed through lookupNativeCode or the JIT's lookup cache.
// Those answer a native code ENTRY, which is a dispatch decision and compiles
// the method as a side effect; this answers the CompiledMethod OBJECT, which
// callers only want to inspect. Same walk, same answer as the .st fallback
// below it, including nil for not-found.
static PrimitiveResult behaviorLookupSelectorPrimitive(Value receiver, Value vSelector)
{
	// EVERY check runs on the tagged Values, BEFORE any asObject. The selector
	// is an ordinary argument, so `lookupSelector: 42` arrives here as an
	// IMMEDIATE, and asObject on an immediate is an assertion failure in a debug
	// build and a wild pointer in a release one. getClassOf is the tag-safe way
	// to ask; anything that is not a Symbol or String falls into the Smalltalk
	// walk below, which answers nil for it.
	RawClass *receiverClass = getClassOf(receiver);
	RawClass *selectorClass = getClassOf(vSelector);
	_Bool isClass = receiverClass != NULL
		&& receiverClass->class == Handles.MetaClass->raw;
	_Bool isMetaClass = receiverClass == Handles.MetaClass->raw;
	if ((!isClass && !isMetaClass)
			|| (selectorClass != Handles.Symbol->raw
				&& selectorClass != Handles.String->raw)) {
		return primFailed();
	}

	HandleScope scope;
	openHandleScope(&scope);
	Class *class = scopeHandle(asObject(receiver));
	String *selector = scopeHandle(asObject(vSelector));
	CompiledMethod *method = lookupSelector(class, selector);
	Value result = getTaggedPtr(method == NULL ? (Object *) Handles.nil : (Object *) method);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult executablePathPrimitive(Value receiver)
{
	char buffer[4096];
	if (!osExecutablePath(buffer, sizeof(buffer))) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	Value result = getTaggedPtr(asString(buffer));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult printHeapPrimitive(Value receiver)
{
	printHeap(CurrentThread.heap);
	return primSuccess(receiver);
}


static _Bool isFloatValue(Value v)
{
	// a Float is either an immediate SmallFloat64 or a boxed heap object
	return valueTypeOf(v, VALUE_FLOAT)
		|| (valueTypeOf(v, VALUE_POINTER) && asObject(v)->class == Handles.BoxedFloat64->raw);
}


// Decode a Float value (immediate or boxed) to its C double. Callers guarantee
// isFloatValue(v), either via the arg guard or because the method is installed
// on a Float class.
static double toDouble(Value v)
{
	if (valueTypeOf(v, VALUE_FLOAT)) {
		return floatValueOf(v);
	}
	return rawFloatValue(asObject(v));
}


static PrimitiveResult floatToInteger(double x)
{
	double max = (double) ((intptr_t) 1 << 60);
	if (x >= -max && x <= max) {
		return primSuccess(tagInt((intptr_t) x));
	}
	return primFailed();
}


/*
 * newFloat() scopes a handle for the freshly allocated Float in the current
 * handle scope so it survives a GC. During a long-running expression (e.g. a
 * hot arithmetic loop) that scope is the top-level activation's and is not
 * closed until it returns, so those transient handles pile up and eventually
 * trip the 256-handle assertion in Handle.h, crashing the VM.
 *
 * A primitive's result is a self-contained tagged pointer, so once we have read
 * it the Float's handle is redundant and can be popped. Popping is valid only
 * because that handle is guaranteed to be the topmost one in the current scope:
 * allocateObject() balances its own inner scope (Heap.c closes it before it
 * returns a raw pointer), so newFloat() adds exactly one handle, and the
 * primitive scopes nothing else in between. The assertions make that invariant
 * explicit and fail loudly if it is ever broken. This keeps float arithmetic
 * O(1) and leak-free, without paying for a full openHandleScope/closeHandleScope
 * (an 8 KB memset) on every operation.
 */
static Value floatResult(Float *object)
{
	HandleScope *scope = CurrentThread.handleScopes;
	Value result = getTaggedPtr(object);

	ASSERT(scope != NULL && scope->size > 0);
	ASSERT((Object *) object == &scope->handles[scope->size - 1]);
	scope->size--;

	return result;
}


// The single choke point for a primitive's Float RESULT: an immediate when the
// double fits, else a fresh box whose scoped handle is popped right away (see
// floatResult above). Only for self-contained results: literal producers
// (Parser.c, Json.c) must instead KEEP the box's handle alive.
static Value fromDoubleResult(double d)
{
	if (smallFloatFits(d)) {
		return tagFloat(d);
	}
	return floatResult(newFloat(d));
}


static PrimitiveResult floatAddPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(toDouble(self) + toDouble(arg)));
}


static PrimitiveResult floatSubPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(toDouble(self) - toDouble(arg)));
}


static PrimitiveResult floatMulPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(toDouble(self) * toDouble(arg)));
}


static PrimitiveResult floatDivPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(toDouble(self) / toDouble(arg)));
}


static PrimitiveResult floatLessThanPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(asBool(toDouble(self) < toDouble(arg))));
}


static PrimitiveResult floatEqualsPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(asBool(toDouble(self) == toDouble(arg))));
}


static PrimitiveResult floatTruncatedPrimitive(Value self)
{
	return floatToInteger(trunc(toDouble(self)));
}


static PrimitiveResult floatFloorPrimitive(Value self)
{
	return floatToInteger(floor(toDouble(self)));
}


static PrimitiveResult floatCeilingPrimitive(Value self)
{
	return floatToInteger(ceil(toDouble(self)));
}


static PrimitiveResult floatRoundedPrimitive(Value self)
{
	return floatToInteger(round(toDouble(self)));
}


static PrimitiveResult floatSqrtPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(sqrt(toDouble(self))));
}


static PrimitiveResult floatSinPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(sin(toDouble(self))));
}


static PrimitiveResult floatCosPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(cos(toDouble(self))));
}


static PrimitiveResult floatExpPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(exp(toDouble(self))));
}


static PrimitiveResult floatLnPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(log(toDouble(self))));
}


static PrimitiveResult floatTanPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(tan(toDouble(self))));
}


static PrimitiveResult floatArcSinPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(asin(toDouble(self))));
}


static PrimitiveResult floatArcCosPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(acos(toDouble(self))));
}


static PrimitiveResult floatArcTanPrimitive(Value self)
{
	return primSuccess(fromDoubleResult(atan(toDouble(self))));
}


static PrimitiveResult floatArcTan2Primitive(Value self, Value arg)
{
	// self arcTan: arg  ==  atan2(self, arg); fall back to coerce a non-Float arg.
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(atan2(toDouble(self), toDouble(arg))));
}


static PrimitiveResult intAsFloatPrimitive(Value self)
{
	return primSuccess(fromDoubleResult((double) asCInt(self)));
}


// Base-2 exponent (ilogb): 1.0 -> 0, 1.0e300 -> 996. Fails for zero, Inf and
// NaN, whose exponent is undefined; the Smalltalk fallback raises.
static PrimitiveResult floatExponentPrimitive(Value self)
{
	double x = toDouble(self);
	if (x == 0.0 || isnan(x) || isinf(x)) {
		return primFailed();
	}
	return primSuccess(tagInt((intptr_t) ilogb(x)));
}


// self * 2^arg, exact (ldexp): the mantissa is untouched, so together with
// FloatExponentPrimitive this lets Smalltalk take a double apart losslessly.
static PrimitiveResult floatTimesTwoPowerPrimitive(Value self, Value arg)
{
	if (!valueTypeOf(arg, VALUE_INT)) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(ldexp(toDouble(self), (int) asCInt(arg))));
}


static PrimitiveResult floatAsStringPrimitive(Value self)
{
	double x = toDouble(self);
	char buf[64];

	if (isnan(x)) {
		strcpy(buf, "nan");
	} else if (isinf(x)) {
		strcpy(buf, x < 0 ? "-inf" : "inf");
	} else {
		jsonFormatDouble(x, buf); /* shared shortest-round-trip form (vm/Json.c) */
	}

	HandleScope scope;
	openHandleScope(&scope);
	Value result = getTaggedPtr(asString(buf));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


/*
 * FloatArray: a BytesShape collection whose bytes ARE native-endian IEEE-754
 * doubles, with no per-element tag and nothing for the GC to scan.
 *
 * Why BytesShape and not a new InstanceShape field: computeInstanceSize knows
 * exactly two element widths (1 byte when isBytes, else 8 for pointer slots),
 * and widening it would break the three places that pun an InstanceShape into a
 * Value plus the hardcoded bit math in Behavior.st. It also would not help:
 * the generic AtPrimitive rebuilds a Value as `(byte << 2) + shape.valueType`,
 * and there is no encoding in that scheme for "make a double out of 8 bytes",
 * so dedicated accessors are needed either way. As bytes, the GC, the snapshot
 * and actor message passing all work with no change at all: they already
 * memcpy byte-shaped indexed contents verbatim, and isFloatShape (which would
 * confuse a BoxedFloat64 with this) requires !isIndexed.
 *
 * The receiver is validated by SHAPE, not by class identity, exactly like
 * replaceBytesPrimitive: these are installed only on FloatArray, and a
 * primitive pointed at the wrong class is a kernel bug like any other. The
 * byte count must be a whole number of doubles, which a String never satisfies
 * by accident for a non-multiple-of-8 length and which costs one test.
 *
 * Alignment: an indexed byte payload starts at offset 24 from a 16-aligned
 * object base, so it is 8-aligned and a double load is never misaligned.
 */
static _Bool floatArrayElements(Value vSelf, double **elements, size_t *count)
{
	if (!valueTypeOf(vSelf, VALUE_POINTER)) {
		return 0;
	}
	RawObject *self = asObject(vSelf);
	InstanceShape shape = self->class->instanceShape;
	if (!shape.isIndexed || !shape.isBytes) {
		return 0;
	}
	size_t bytes = rawObjectSize(self);
	if (bytes % sizeof(double) != 0) {
		return 0;
	}
	*elements = (double *) getRawObjectIndexedVars(self);
	*count = bytes / sizeof(double);
	return 1;
}


// Accepts any Number the tower can represent exactly as a double operand: a
// Float (immediate or boxed) or a SmallInteger, matching what the mixed
// arithmetic path coerces. Anything else fails into the Smalltalk fallback,
// which raises the precise error.
static _Bool floatArrayValue(Value v, double *out)
{
	if (isFloatValue(v)) {
		*out = toDouble(v);
		return 1;
	}
	if (valueTypeOf(v, VALUE_INT)) {
		*out = (double) asCInt(v);
		return 1;
	}
	return 0;
}


static PrimitiveResult floatArrayAtPrimitive(Value vSelf, Value vIndex)
{
	double *elements;
	size_t count;
	if (!floatArrayElements(vSelf, &elements, &count) || !valueTypeOf(vIndex, VALUE_INT)) {
		return primFailed();
	}
	intptr_t index = asCInt(vIndex);
	if (index < 1 || (size_t) index > count) {
		return primFailed();
	}
	return primSuccess(fromDoubleResult(elements[index - 1]));
}


static PrimitiveResult floatArrayAtPutPrimitive(Value vSelf, Value vIndex, Value vValue)
{
	double *elements;
	size_t count;
	double value;
	if (!floatArrayElements(vSelf, &elements, &count) || !valueTypeOf(vIndex, VALUE_INT)
			|| !floatArrayValue(vValue, &value)) {
		return primFailed();
	}
	intptr_t index = asCInt(vIndex);
	if (index < 1 || (size_t) index > count) {
		return primFailed();
	}
	elements[index - 1] = value;
	// No write barrier: the payload holds raw doubles, never object pointers,
	// which is the whole point of the shape.
	return primSuccess(vValue);
}


// Reductions. These are where the win is: no per-element send, no tag decode,
// no boxing of intermediates, and nothing for the collector to scan.
enum {
	FLOAT_ARRAY_SUM = 1,
	FLOAT_ARRAY_MAX = 2,
	FLOAT_ARRAY_MIN = 3,
};

static PrimitiveResult floatArrayReducePrimitive(Value vSelf, Value vOp)
{
	double *elements;
	size_t count;
	if (!floatArrayElements(vSelf, &elements, &count) || !valueTypeOf(vOp, VALUE_INT)) {
		return primFailed();
	}
	intptr_t op = asCInt(vOp);
	if (op == FLOAT_ARRAY_SUM) {
		double sum = 0.0;
		for (size_t i = 0; i < count; i++) {
			sum += elements[i];
		}
		return primSuccess(fromDoubleResult(sum));
	}
	// max and min have no identity element, so an empty array must fail into
	// the Smalltalk fallback, which raises EmptyCollectionError.
	if (count == 0) {
		return primFailed();
	}
	double best = elements[0];
	for (size_t i = 1; i < count; i++) {
		// NaN-propagating comparison would answer whichever end it started
		// from; keep it simple and match Collection>>max, which uses <.
		if (op == FLOAT_ARRAY_MAX ? elements[i] > best : elements[i] < best) {
			best = elements[i];
		}
	}
	return primSuccess(fromDoubleResult(best));
}


static PrimitiveResult floatArrayDotPrimitive(Value vSelf, Value vOther)
{
	double *a, *b;
	size_t countA, countB;
	if (!floatArrayElements(vSelf, &a, &countA) || !floatArrayElements(vOther, &b, &countB)
			|| countA != countB) {
		return primFailed();
	}
	double sum = 0.0;
	for (size_t i = 0; i < countA; i++) {
		sum += a[i] * b[i];
	}
	return primSuccess(fromDoubleResult(sum));
}


// Elementwise and scalar arithmetic. The DESTINATION is allocated in Smalltalk
// and passed in, so these stay pure C loops over three buffers with no
// allocation and therefore no GC interaction at all. One primitive with an
// opcode rather than four: the table is append-only forever, so a family of
// near-identical kernels is exactly the case for spending one slot.
enum {
	FLOAT_ARRAY_ADD = 1,
	FLOAT_ARRAY_SUB = 2,
	FLOAT_ARRAY_MUL = 3,
	FLOAT_ARRAY_DIV = 4,
};

static double floatArrayApply(intptr_t op, double x, double y)
{
	switch (op) {
	case FLOAT_ARRAY_ADD: return x + y;
	case FLOAT_ARRAY_SUB: return x - y;
	case FLOAT_ARRAY_MUL: return x * y;
	default:              return x / y;  // FLOAT_ARRAY_DIV, IEEE: /0 is Inf/NaN
	}
}


static PrimitiveResult floatArrayBinaryPrimitive(Value vSelf, Value vOther, Value vDest, Value vOp)
{
	double *a, *b, *dest;
	size_t countA, countB, countDest;
	if (!floatArrayElements(vSelf, &a, &countA) || !floatArrayElements(vOther, &b, &countB)
			|| !floatArrayElements(vDest, &dest, &countDest)
			|| !valueTypeOf(vOp, VALUE_INT)
			|| countA != countB || countA != countDest) {
		return primFailed();
	}
	intptr_t op = asCInt(vOp);
	if (op < FLOAT_ARRAY_ADD || op > FLOAT_ARRAY_DIV) {
		return primFailed();
	}
	for (size_t i = 0; i < countA; i++) {
		dest[i] = floatArrayApply(op, a[i], b[i]);
	}
	return primSuccess(vDest);
}


static PrimitiveResult floatArrayScalarPrimitive(Value vSelf, Value vScalar, Value vDest, Value vOp)
{
	double *a, *dest;
	size_t countA, countDest;
	double scalar;
	if (!floatArrayElements(vSelf, &a, &countA) || !floatArrayElements(vDest, &dest, &countDest)
			|| !floatArrayValue(vScalar, &scalar) || !valueTypeOf(vOp, VALUE_INT)
			|| countA != countDest) {
		return primFailed();
	}
	intptr_t op = asCInt(vOp);
	if (op < FLOAT_ARRAY_ADD || op > FLOAT_ARRAY_DIV) {
		return primFailed();
	}
	for (size_t i = 0; i < countA; i++) {
		dest[i] = floatArrayApply(op, a[i], scalar);
	}
	return primSuccess(vDest);
}


static PrimitiveResult floatArrayFillPrimitive(Value vSelf, Value vValue)
{
	double *elements;
	size_t count;
	double value;
	if (!floatArrayElements(vSelf, &elements, &count) || !floatArrayValue(vValue, &value)) {
		return primFailed();
	}
	for (size_t i = 0; i < count; i++) {
		elements[i] = value;
	}
	return primSuccess(vSelf);
}


/*
 * LargeInteger over the magnitude kernels (vm/runtime/BigInt.c).
 *
 * REPRESENTATION. A LargeInteger is a BytesShape object whose byte payload IS
 * the magnitude: an array of 32-bit limbs, least significant first, in native
 * order. The sign is the CONCRETE CLASS (LargePositiveInteger /
 * LargeNegativeInteger), Pharo style, so there is no sign field to keep
 * consistent. The image used an Array of TAGGED SmallInteger limbs in base
 * 2^16, which cost 8 bytes of GC-scanned heap per 16 bits of value; as bytes it
 * is 4 bytes per 32 bits and the collector never looks inside.
 *
 * Because it is byte-shaped, the snapshot and actor message passing already
 * round-trip it verbatim, exactly as for FloatArray, with no work here.
 *
 * NORMALIZATION is done in C, on every result: a magnitude that fits the
 * 62-bit signed payload comes back as a tagged SmallInteger instead. The
 * threshold is ASYMMETRIC ([-2^61, 2^61-1]) and MUST agree with
 * SmallInteger class maxVal/minVal, with tagInt's assertion, and with the
 * parser's literal builder, or the same magnitude gets a different class
 * depending on how it was produced.
 *
 * The two concrete classes arrive as a 2-element Array argument rather than
 * through Handles: adding Handles entries is append-only and bumps the snapshot
 * format, and looking the globals up by name per operation would be a hash
 * probe on the hot path. LargeInteger holds the pair in a class variable.
 */
#define LARGE_LIMB_BITS 32

// Read a SmallInteger or a LargeInteger as (limbs, count, negative). The
// SmallInteger case materializes into the caller's 2-limb scratch, which is
// what lets every operation accept a mixed pair without the image converting
// first.
static _Bool largeIntOperand(Value v, uint32_t *scratch, const uint32_t **limbs,
	size_t *count, _Bool *negative)
{
	if (valueTypeOf(v, VALUE_INT)) {
		intptr_t i = asCInt(v);
		*negative = i < 0;
		// Negate through uint64_t: the magnitude of the most negative value is
		// one past the positive range, so negating in intptr_t would overflow.
		uint64_t magnitude = *negative ? (uint64_t) (-(int64_t) i) : (uint64_t) i;
		scratch[0] = (uint32_t) (magnitude & 0xFFFFFFFFu);
		scratch[1] = (uint32_t) (magnitude >> LARGE_LIMB_BITS);
		*limbs = scratch;
		*count = bigIntNormalize(scratch, 2);
		return 1;
	}
	if (!valueTypeOf(v, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(v);
	InstanceShape shape = object->class->instanceShape;
	if (!shape.isIndexed || !shape.isBytes) {
		return 0;
	}
	size_t bytes = rawObjectSize(object);
	if (bytes % sizeof(uint32_t) != 0) {
		return 0;
	}
	*limbs = (const uint32_t *) getRawObjectIndexedVars(object);
	*count = bigIntNormalize(*limbs, bytes / sizeof(uint32_t));
	// The sign is the class. Anything byte-shaped that is not one of the two
	// concrete LargeInteger classes is rejected by the caller's class check.
	*negative = 0;
	return 1;
}


// True when the class of `v` is the negative concrete class. Kept separate
// from largeIntOperand because it needs the classes the image passed in.
static _Bool largeIntIsNegative(Value v, RawObject *negativeClass)
{
	if (valueTypeOf(v, VALUE_INT)) {
		return asCInt(v) < 0;
	}
	return asObject(v)->class == (RawClass *) negativeClass;
}


// Does this magnitude fit the 62-bit signed payload? ASYMMETRIC on purpose.
static _Bool largeIntFitsSmall(const uint32_t *limbs, size_t n, _Bool negative)
{
	if (n > 2) {
		return 0;
	}
	uint64_t magnitude = 0;
	if (n >= 1) {
		magnitude |= limbs[0];
	}
	if (n == 2) {
		magnitude |= (uint64_t) limbs[1] << LARGE_LIMB_BITS;
	}
	uint64_t limit = (uint64_t) 1 << 61;
	return negative ? magnitude <= limit : magnitude < limit;
}


// Build the image-level result: a tagged SmallInteger when it fits, else a
// fresh byte-shaped LargeInteger of the sign's concrete class.
static Value largeIntResult(const uint32_t *limbs, size_t n, _Bool negative, Value vClasses)
{
	n = bigIntNormalize(limbs, n);
	if (n == 0) {
		return tagInt(0); // zero has no sign
	}
	if (largeIntFitsSmall(limbs, n, negative)) {
		uint64_t magnitude = limbs[0];
		if (n == 2) {
			magnitude |= (uint64_t) limbs[1] << LARGE_LIMB_BITS;
		}
		return tagInt(negative ? -(intptr_t) magnitude : (intptr_t) magnitude);
	}
	HandleScope scope;
	openHandleScope(&scope);
	// Scope the class BEFORE allocating: newObject can collect, and `limbs`
	// points into a malloc'd buffer so only the class needs protecting.
	RawArray *classes = (RawArray *) asObject(vClasses);
	Class *class = (Class *) scopeHandle(asObject(classes->vars[negative ? 1 : 0]));
	Object *result = newObject(class, n * sizeof(uint32_t));
	memcpy(getObjectIndexedVars(result), limbs, n * sizeof(uint32_t));
	Value tagged = getTaggedPtr(result);
	closeHandleScope(&scope, NULL);
	return tagged;
}


enum {
	LARGE_OP_ADD = 1,
	LARGE_OP_SUB = 2,
	LARGE_OP_MUL = 3,
	LARGE_OP_QUO = 4,   // truncating quotient
	LARGE_OP_REM = 5,   // remainder of QUO, sign of the dividend
	LARGE_OP_DIV = 6,   // floored quotient
	LARGE_OP_MOD = 7,   // remainder of DIV, sign of the divisor
	LARGE_OP_GCD = 8,
};


static PrimitiveResult largeIntBinaryPrimitive(Value vSelf, Value vOther, Value vOp,
	Value vClasses)
{
	if (!valueTypeOf(vOp, VALUE_INT) || !valueTypeOf(vClasses, VALUE_POINTER)) {
		return primFailed();
	}
	uint32_t scratchA[2], scratchB[2];
	const uint32_t *a, *b;
	size_t na, nb;
	_Bool ignoredA, ignoredB;
	if (!largeIntOperand(vSelf, scratchA, &a, &na, &ignoredA)
			|| !largeIntOperand(vOther, scratchB, &b, &nb, &ignoredB)) {
		return primFailed();
	}
	RawArray *classes = (RawArray *) asObject(vClasses);
	RawObject *negativeClass = asObject(classes->vars[1]);
	_Bool negA = largeIntIsNegative(vSelf, negativeClass);
	_Bool negB = largeIntIsNegative(vOther, negativeClass);
	intptr_t op = asCInt(vOp);

	// Worst case across every operation: a product needs na + nb limbs, a sum
	// needs max + 1, a shifted intermediate a little more.
	size_t cap = na + nb + 4;
	uint32_t *out = calloc(cap, sizeof(uint32_t));
	if (out == NULL) {
		FAIL();
	}
	size_t nOut = 0;
	_Bool negOut = 0;
	_Bool ok = 1;

	switch (op) {
	case LARGE_OP_ADD:
	case LARGE_OP_SUB: {
		_Bool effB = op == LARGE_OP_SUB ? !negB : negB;
		if (negA == effB) {
			nOut = bigIntAdd(a, na, b, nb, out);
			negOut = negA;
		} else if (bigIntCompare(a, na, b, nb) >= 0) {
			nOut = bigIntSub(a, na, b, nb, out);
			negOut = negA;
		} else {
			nOut = bigIntSub(b, nb, a, na, out);
			negOut = effB;
		}
		break;
	}

	case LARGE_OP_MUL:
		nOut = bigIntMul(a, na, b, nb, out);
		negOut = negA != negB;
		break;

	case LARGE_OP_QUO:
	case LARGE_OP_REM:
	case LARGE_OP_DIV:
	case LARGE_OP_MOD: {
		if (nb == 0) {
			ok = 0; // division by zero: the image raises ZeroDivide
			break;
		}
		uint32_t *q = calloc(na + 1, sizeof(uint32_t));
		uint32_t *r = calloc(nb + 1, sizeof(uint32_t));
		if (q == NULL || r == NULL) {
			FAIL();
		}
		size_t nq = 0, nr = 0;
		bigIntDivMod(a, na, b, nb, q, &nq, r, &nr);
		_Bool negQ = negA != negB;
		if (op == LARGE_OP_QUO) {
			memcpy(out, q, nq * sizeof(uint32_t));
			nOut = nq;
			negOut = negQ;
		} else if (op == LARGE_OP_REM) {
			memcpy(out, r, nr * sizeof(uint32_t));
			nOut = nr;
			negOut = negA; // remainder takes the DIVIDEND's sign
		} else {
			// Floored forms: when the signs differ and the division was not
			// exact, the quotient goes one further from zero and the remainder
			// swings to the divisor's sign.
			_Bool adjust = negQ && nr != 0;
			if (op == LARGE_OP_DIV) {
				if (adjust) {
					uint32_t one = 1;
					nOut = bigIntAdd(q, nq, &one, 1, out);
				} else {
					memcpy(out, q, nq * sizeof(uint32_t));
					nOut = nq;
				}
				negOut = negQ;
			} else {
				if (adjust) {
					nOut = bigIntSub(b, nb, r, nr, out);
				} else {
					memcpy(out, r, nr * sizeof(uint32_t));
					nOut = nr;
				}
				negOut = negB; // modulo takes the DIVISOR's sign
			}
		}
		free(q);
		free(r);
		break;
	}

	case LARGE_OP_GCD:
		nOut = bigIntGcd(a, na, b, nb, out);
		negOut = 0; // a gcd is non-negative
		break;

	default:
		ok = 0;
		break;
	}

	Value result = 0;
	if (ok) {
		result = largeIntResult(out, nOut, negOut, vClasses);
	}
	free(out);
	return ok ? primSuccess(result) : primFailed();
}


static PrimitiveResult largeIntComparePrimitive(Value vSelf, Value vOther, Value vClasses)
{
	if (!valueTypeOf(vClasses, VALUE_POINTER)) {
		return primFailed();
	}
	uint32_t scratchA[2], scratchB[2];
	const uint32_t *a, *b;
	size_t na, nb;
	_Bool ignoredA, ignoredB;
	if (!largeIntOperand(vSelf, scratchA, &a, &na, &ignoredA)
			|| !largeIntOperand(vOther, scratchB, &b, &nb, &ignoredB)) {
		return primFailed();
	}
	RawArray *classes = (RawArray *) asObject(vClasses);
	RawObject *negativeClass = asObject(classes->vars[1]);
	_Bool negA = largeIntIsNegative(vSelf, negativeClass);
	_Bool negB = largeIntIsNegative(vOther, negativeClass);
	if (na == 0) {
		negA = 0;
	}
	if (nb == 0) {
		negB = 0;
	}
	int result;
	if (negA != negB) {
		result = negA ? -1 : 1;
	} else {
		result = bigIntCompare(a, na, b, nb);
		if (negA) {
			result = -result;
		}
	}
	return primSuccess(tagInt(result));
}


// Arithmetic shift. A negative count on a NEGATIVE receiver must floor, which
// sign-magnitude does not do for free: shifting the magnitude truncates toward
// zero, so one is subtracted when any bit was shifted out.
static PrimitiveResult largeIntShiftPrimitive(Value vSelf, Value vShift, Value vClasses)
{
	if (!valueTypeOf(vShift, VALUE_INT) || !valueTypeOf(vClasses, VALUE_POINTER)) {
		return primFailed();
	}
	uint32_t scratch[2];
	const uint32_t *a;
	size_t na;
	_Bool ignored;
	if (!largeIntOperand(vSelf, scratch, &a, &na, &ignored)) {
		return primFailed();
	}
	RawArray *classes = (RawArray *) asObject(vClasses);
	_Bool negative = largeIntIsNegative(vSelf, asObject(classes->vars[1]));
	intptr_t shift = asCInt(vShift);

	size_t cap = na + (shift > 0 ? (size_t) shift / LARGE_LIMB_BITS : 0) + 4;
	uint32_t *out = calloc(cap, sizeof(uint32_t));
	if (out == NULL) {
		FAIL();
	}
	size_t nOut;
	if (shift >= 0) {
		nOut = bigIntShiftLeft(a, na, (size_t) shift, out);
	} else {
		size_t amount = (size_t) (-shift);
		nOut = bigIntShiftRight(a, na, amount, out);
		if (negative) {
			// Did anything fall off the bottom? If so the truncated magnitude
			// is one short of the floored result.
			_Bool lost = 0;
			for (size_t bit = 0; bit < amount && bit < na * LARGE_LIMB_BITS; bit++) {
				if (bigIntBitAt(a, na, bit)) {
					lost = 1;
					break;
				}
			}
			if (lost) {
				uint32_t one = 1;
				uint32_t *bumped = calloc(nOut + 2, sizeof(uint32_t));
				if (bumped == NULL) {
					FAIL();
				}
				size_t nBumped = bigIntAdd(out, nOut, &one, 1, bumped);
				memcpy(out, bumped, nBumped * sizeof(uint32_t));
				nOut = nBumped;
				free(bumped);
			}
		}
	}
	Value result = largeIntResult(out, nOut, negative, vClasses);
	free(out);
	return primSuccess(result);
}


static PrimitiveResult largeIntAsFloatPrimitive(Value vSelf, Value vClasses)
{
	if (!valueTypeOf(vClasses, VALUE_POINTER)) {
		return primFailed();
	}
	uint32_t scratch[2];
	const uint32_t *a;
	size_t na;
	_Bool ignored;
	if (!largeIntOperand(vSelf, scratch, &a, &na, &ignored)) {
		return primFailed();
	}
	RawArray *classes = (RawArray *) asObject(vClasses);
	_Bool negative = largeIntIsNegative(vSelf, asObject(classes->vars[1]));
	double d = bigIntToDouble(a, na);
	return primSuccess(fromDoubleResult(negative ? -d : d));
}


// Digits only, WITHOUT a sign: the image prepends '-' so the same primitive
// serves printString and printString:.
static PrimitiveResult largeIntPrintStringPrimitive(Value vSelf, Value vBase)
{
	if (!valueTypeOf(vBase, VALUE_INT)) {
		return primFailed();
	}
	intptr_t base = asCInt(vBase);
	if (base < 2 || base > 36) {
		return primFailed();
	}
	uint32_t scratch[2];
	const uint32_t *a;
	size_t na;
	_Bool ignored;
	if (!largeIntOperand(vSelf, scratch, &a, &na, &ignored)) {
		return primFailed();
	}
	size_t cap = bigIntStringSize(na, (int) base);
	char *buffer = malloc(cap);
	if (buffer == NULL) {
		FAIL();
	}
	size_t len = bigIntToString(a, na, (int) base, buffer, cap);
	HandleScope scope;
	openHandleScope(&scope);
	String *string = newString(len);
	memcpy(string->raw->contents, buffer, len);
	Value result = getTaggedPtr(string);
	closeHandleScope(&scope, NULL);
	free(buffer);
	return primSuccess(result);
}


static PrimitiveResult largeIntFromStringPrimitive(Value vDigits, Value vBase,
	Value vNegative, Value vClasses)
{
	if (!valueTypeOf(vDigits, VALUE_POINTER) || !valueTypeOf(vBase, VALUE_INT)
			|| !valueTypeOf(vClasses, VALUE_POINTER)) {
		return primFailed();
	}
	intptr_t base = asCInt(vBase);
	if (base < 2 || base > 36) {
		return primFailed();
	}
	RawObject *digits = asObject(vDigits);
	if (!digits->class->instanceShape.isBytes) {
		return primFailed();
	}
	size_t len = rawObjectSize(digits);
	size_t cap = bigIntFromStringSize(len, (int) base);
	uint32_t *out = calloc(cap, sizeof(uint32_t));
	if (out == NULL) {
		FAIL();
	}
	size_t n = bigIntFromString((const char *) getRawObjectIndexedVars(digits), len,
		(int) base, out);
	Value result = largeIntResult(out, n, vNegative == getTaggedPtr(Handles.true), vClasses);
	free(out);
	return primSuccess(result);
}


static PrimitiveResult lastGcStatsPrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);
	Dictionary *stats = newDictionary(16);
	stringDictAtPut(stats, asString("count"), tagInt(LastGCStats.count));
	stringDictAtPut(stats, asString("total"), tagInt(LastGCStats.total));
	stringDictAtPut(stats, asString("marked"), tagInt(LastGCStats.marked));
	stringDictAtPut(stats, asString("sweeped"), tagInt(LastGCStats.sweeped));
	stringDictAtPut(stats, asString("freed"), tagInt(LastGCStats.freed));
	stringDictAtPut(stats, asString("extended"), tagInt(LastGCStats.extended));
	stringDictAtPut(stats, asString("fullTimeUs"), tagInt(LastGCStats.totalTime));
	stringDictAtPut(stats, asString("scavengeCount"), tagInt(LastGCStats.scavengeCount));
	stringDictAtPut(stats, asString("scavengeTimeUs"), tagInt(LastGCStats.scavengeTimeUs));
	stringDictAtPut(stats, asString("oldBytes"), tagInt(CurrentThread.heap->oldSpace.totalBytes));
	stringDictAtPut(stats, asString("remembered"), tagInt(rememberedSetCount(&CurrentThread.rememberedSet)));
	stringDictAtPut(stats, asString("youngSurvivorBytes"), tagInt(LastGCStats.youngSurvivorBytes));
	stringDictAtPut(stats, asString("liveFibers"), tagInt(schedulerLiveFibers()));
	stringDictAtPut(stats, asString("fiberSlots"), tagInt(schedulerFiberSlots()));
	stringDictAtPut(stats, asString("armedWaiters"), tagInt(schedulerArmedWaiters()));
	Value result = getTaggedPtr(stats);
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


// ---- OS/system primitives (stdlib core) -----------------------------------

static int commandLineCount = 0;
static char **commandLineArguments = NULL;


void primitivesSetCommandLine(int count, char **arguments)
{
	commandLineCount = count;
	commandLineArguments = arguments;
}


// Marshal a String/Symbol argument into a NUL-terminated C path buffer.
// Rejects non-byte objects and paths that do not fit (with the NUL).
static _Bool pathFromValue(Value vPath, char *buffer, size_t capacity)
{
	if (!valueTypeOf(vPath, VALUE_POINTER)) {
		return 0;
	}
	RawObject *raw = asObject(vPath);
	if (!raw->class->instanceShape.isBytes) {
		return 0;
	}
	size_t size = rawObjectSize(raw);
	if (size >= capacity) {
		return 0;
	}
	memcpy(buffer, getRawObjectIndexedVars(raw), size);
	buffer[size] = '\0';
	return 1;
}


static PrimitiveResult monotonicNanoTimePrimitive(Value receiver)
{
	return primSuccess(tagInt(osMonotonicNanos()));
}


static PrimitiveResult localUtcOffsetPrimitive(Value receiver, Value epochSeconds)
{
	if (!valueTypeOf(epochSeconds, VALUE_INT)) {
		return primFailed();
	}
	return primSuccess(tagInt(osLocalUtcOffsetSeconds(asCInt(epochSeconds))));
}


// Fill the whole ByteArray with OS entropy, in place: no allocation, no GC
// window. Answers the receiver's argument so .st can chain on it.
static PrimitiveResult randomBytesPrimitive(Value receiver, Value bytes)
{
	if (!valueTypeOf(bytes, VALUE_POINTER)) {
		return primFailed();
	}
	RawObject *raw = asObject(bytes);
	if (!raw->class->instanceShape.isBytes) {
		return primFailed();
	}
	if (!osRandomBytes(getRawObjectIndexedVars(raw), rawObjectSize(raw))) {
		return primFailed();
	}
	return primSuccess(bytes);
}


static PrimitiveResult getEnvPrimitive(Value receiver, Value name)
{
	char nameBuffer[256];
	if (!pathFromValue(name, nameBuffer, sizeof(nameBuffer))) {
		return primFailed();
	}
	char *value = getenv(nameBuffer);
	if (value == NULL) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	HandleScope scope;
	openHandleScope(&scope);
	Value result = getTaggedPtr(asString(value));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult commandLinePrimitive(Value receiver)
{
	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *result = newOrdColl(commandLineCount > 0 ? (size_t) commandLineCount : 1);
	for (int i = 0; i < commandLineCount; i++) {
		HandleScope entry;
		openHandleScope(&entry);
		ordCollAddObject(result, (Object *) asString(commandLineArguments[i]));
		closeHandleScope(&entry, NULL);
	}
	Value r = getTaggedPtr(result);
	closeHandleScope(&scope, NULL);
	return primSuccess(r);
}


static PrimitiveResult exitWithCodePrimitive(Value receiver, Value code)
{
	exit(valueTypeOf(code, VALUE_INT) ? (int) asCInt(code) : EXIT_FAILURE);
}


static PrimitiveResult cpuCountPrimitive(Value receiver)
{
	return primSuccess(tagInt(osAvailableCoreCount()));
}


// Answers {type. size. mtimeMillis} (type 1 = file, 2 = directory, 3 = other)
// or nil when the path cannot be stat'ed; `exists` in .st is stat notNil.
static PrimitiveResult fileStatPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	int64_t info[3];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	if (!fsStatPath(pathBuffer, info)) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	HandleScope scope;
	openHandleScope(&scope);
	Array *result = newArray(3);
	result->raw->vars[0] = tagInt(info[0]);
	result->raw->vars[1] = tagInt(info[1]);
	result->raw->vars[2] = tagInt(info[2]);
	Value r = getTaggedPtr(result);
	closeHandleScope(&scope, NULL);
	return primSuccess(r);
}


static PrimitiveResult fileMkdirPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(fsMkdir(pathBuffer) ? Handles.true : Handles.false));
}


static PrimitiveResult fileUnlinkPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(fsUnlink(pathBuffer) ? Handles.true : Handles.false));
}


static PrimitiveResult fileRmdirPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(fsRmdir(pathBuffer) ? Handles.true : Handles.false));
}


static PrimitiveResult fileRenamePrimitive(Value receiver, Value from, Value to)
{
	char fromBuffer[FS_PATH_MAX];
	char toBuffer[FS_PATH_MAX];
	if (!pathFromValue(from, fromBuffer, sizeof(fromBuffer))) {
		return primFailed();
	}
	if (!pathFromValue(to, toBuffer, sizeof(toBuffer))) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(fsRename(fromBuffer, toBuffer) ? Handles.true : Handles.false));
}


// Answers an OrderedCollection of entry names, or nil when the directory
// cannot be read (missing, not a directory, no permission).
static PrimitiveResult fileListDirPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *result = fsListDir(pathBuffer);
	if (result == NULL) {
		closeHandleScope(&scope, NULL);
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	Value r = getTaggedPtr(result);
	closeHandleScope(&scope, NULL);
	return primSuccess(r);
}


static PrimitiveResult fileGetCwdPrimitive(Value receiver)
{
	char pathBuffer[FS_PATH_MAX];
	if (!fsGetCwd(pathBuffer, sizeof(pathBuffer))) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	HandleScope scope;
	openHandleScope(&scope);
	Value result = getTaggedPtr(asString(pathBuffer));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}


static PrimitiveResult fileChdirPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(fsChdir(pathBuffer) ? Handles.true : Handles.false));
}


static PrimitiveResult fileRealpathPrimitive(Value receiver, Value path)
{
	char pathBuffer[FS_PATH_MAX];
	char resolved[FS_PATH_MAX];
	if (!pathFromValue(path, pathBuffer, sizeof(pathBuffer))) {
		return primFailed();
	}
	if (!fsRealpath(pathBuffer, resolved)) {
		return primSuccess(getTaggedPtr(Handles.nil));
	}
	HandleScope scope;
	openHandleScope(&scope);
	Value result = getTaggedPtr(asString(resolved));
	closeHandleScope(&scope, NULL);
	return primSuccess(result);
}
