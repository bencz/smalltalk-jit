#include "Primitives.h"
#include "CodeGenerator.h"
#include "Object.h"
#include "Class.h"
#include "Heap.h"
#include "Exception.h"
#include "Smalltalk.h"
#include "Compiler.h"
#include "Stream.h"
#include "Socket.h"
#include "Parser.h"
#include "Lookup.h"
#include "StackFrame.h"
#include "Thread.h"
#include "Handle.h"
#include "GarbageCollector.h"
#include "Entry.h"
#include "Os.h"
#include "Scheduler.h"
#include "Message.h"
#include "Collection.h"
#include "Assert.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

typedef struct {
	Value value;
	intptr_t failed;
} PrimitiveResult;

typedef struct {
	char *name;
	enum { GEN, CCALL } type;
	union {
		void (*generate)(CodeGenerator *);
		PrimitiveResult (*cFunction)();
	};
	uint8_t numArgs;
} Primitive;

static PrimitiveResult primSuccess(Value resultValue);
static PrimitiveResult primFailed();
static PrimitiveResult arrayEqualsPrimitive(Value receiver, Value operand);
static PrimitiveResult replaceBytesPrimitive(Value self, Value start, Value stop, Value replacement, Value replacementStart);
static PrimitiveResult indexOfBytePrimitive(Value self, Value byte, Value start);
static PrimitiveResult becomePrimitive(Value object, Value other);
static PrimitiveResult workerParallelPrimitive(Value self, Value blocks);
static PrimitiveResult contextPositionDescriptorPrimitive(Value vContext);
static PrimitiveResult stringAsSymbolPrimitive(Value receiver);
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
static PrimitiveResult printHeapPrimitive(Value receiver);
static PrimitiveResult lastGcStatsPrimitive(Value receiver);
static PrimitiveResult processSpawnPrimitive(Value block);
static PrimitiveResult processResumePrimitive(Value self, Value id);
static PrimitiveResult processYieldPrimitive(Value self);
static PrimitiveResult processTerminatePrimitive(Value self, Value id);
static PrimitiveResult processCurrentIdPrimitive(Value self);
static PrimitiveResult processSuspendPrimitive(Value self);
static PrimitiveResult processSleepPrimitive(Value self, Value micros);
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

#include "PrimitivesX64.c"

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

	{"WorkerParallelPrimitive", CCALL, .cFunction = workerParallelPrimitive, 2},

	{"ParseClassPrimitive", CCALL, .cFunction = parseClassPrimitive, 1},
	{"ParseMethodPrimitive", CCALL, .cFunction = parseMethodPrimitive, 1},
	{"ParseMethodOrBlockPrimitive", CCALL, .cFunction = parseMethodOrBlockPrimitive, 1},

	{"BuildClassPrimitive", CCALL, .cFunction = buildClassPrimitive, 2},
	{"CompileMethodPrimitive", CCALL, .cFunction = compileMethodPrimitive, 3},
	{"MethodSendPrimitive", GEN, generateMethodSendPrimitive},
	{"MethodSendArgsPrimitive", GEN, generateMethodSendArgsPrimitive},
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


static PrimitiveResult primSuccess(Value resultValue)
{
	PrimitiveResult result = { .value = resultValue, .failed = 0 };
	return result;
}


static PrimitiveResult primFailed()
{
	PrimitiveResult result = { .failed = 1 };
	return result;
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

static void *parallelPrimWorker(void *arg)
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
	return NULL;
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
	pthread_t *threads = malloc(n * sizeof(pthread_t));
	for (size_t i = 0; i < n; i++) {
		works[i].arrayHandle = arrPH;
		works[i].resultsHandle = resultsPH;
		works[i].index = i;
		works[i].handles = mainHandles;
		works[i].heap = heap;
	}
	heapGcEnterBlocked(heap, &CurrentThread); // caller is idle (blocked) during the join
	for (size_t i = 0; i < n; i++) {
		pthread_create(&threads[i], NULL, parallelPrimWorker, &works[i]);
	}
	for (size_t i = 0; i < n; i++) {
		pthread_join(threads[i], NULL);
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
	int descriptor = socketConnect(asCInt(addr->address), asCInt(port));
	return descriptor < 0 ? primFailed() : primSuccess(tagInt(descriptor));
}


static PrimitiveResult socketBindPrimitive(Value socket, Value vAddr, Value port, Value queueSize)
{
	if (!validInternetAddress(vAddr) || !valueTypeOf(port, VALUE_INT) || !valueTypeOf(queueSize, VALUE_INT)) {
		return primFailed();
	}
	RawInternetAddress *addr = (RawInternetAddress *) asObject(vAddr);
	int descriptor = socketBind(asCInt(addr->address), asCInt(port), asCInt(queueSize));
	return descriptor < 0 ? primFailed() : primSuccess(tagInt(descriptor));
}


static PrimitiveResult socketAcceptPrimitive(Value socket)
{
	RawServerSocket *server = (RawServerSocket *) asObject(socket);
	int descriptor = socketAccept(asCInt(server->descriptor));
	return descriptor < 0 ? primFailed() : primSuccess(tagInt(descriptor));
}


// Explicitly disable Nagle on a descriptor. Connected/accepted sockets already
// get this automatically; this primitive is for explicit control from Smalltalk.
static PrimitiveResult socketSetNoDelayPrimitive(Value self, Value vFd)
{
	socketSetNoDelay((int) asCInt(vFd));
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
	int fd = (int) asCInt(vFd);
	intptr_t size = asCInt(vSize);
	intptr_t start = asCInt(vStart) - 1;

	if (start < 0 || start >= buffer->raw->size) {
		closeHandleScope(&scope, NULL);
		return primFailed();
	}
	if (size > buffer->raw->size - start) {
		size = buffer->raw->size - start;
	}

	ptrdiff_t n;
	for (;;) {
		n = read(fd, buffer->raw->contents + start, size);
		if (n >= 0) {
			break;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			schedulerWaitFd(fd, 0);
			continue;
		}
		if (errno == EINTR) {
			continue;
		}
		break;
	}

	closeHandleScope(&scope, NULL);
	return n < 0 ? primFailed() : primSuccess(tagInt(n));
}


// Write exactly `size` bytes from `buffer`, parking the fiber whenever the
// socket send buffer is full. Returns the number of bytes written.
static PrimitiveResult socketWritePrimitive(Value self, Value vFd, Value vBuffer, Value vSize)
{
	HandleScope scope;
	openHandleScope(&scope);

	String *buffer = scopeHandle(asObject(vBuffer));
	int fd = (int) asCInt(vFd);
	intptr_t size = asCInt(vSize);

	if (size > buffer->raw->size) {
		size = buffer->raw->size;
	}

	intptr_t total = 0;
	while (total < size) {
		ptrdiff_t n = write(fd, buffer->raw->contents + total, size - total);
		if (n >= 0) {
			total += n;
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			schedulerWaitFd(fd, 1);
			continue;
		}
		if (errno == EINTR) {
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
		streamSetPosition(asCInt(stream->raw->descriptor), currentToken(&parser->tokenizer)->position - 1);
		fclose(file);
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


static PrimitiveResult printHeapPrimitive(Value receiver)
{
	printHeap(CurrentThread.heap);
	return primSuccess(receiver);
}


static _Bool isFloatValue(Value v)
{
	return valueTypeOf(v, VALUE_POINTER) && asObject(v)->class == Handles.Float->raw;
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


static PrimitiveResult floatAddPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(floatResult(newFloat(rawFloatValue(asObject(self)) + rawFloatValue(asObject(arg)))));
}


static PrimitiveResult floatSubPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(floatResult(newFloat(rawFloatValue(asObject(self)) - rawFloatValue(asObject(arg)))));
}


static PrimitiveResult floatMulPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(floatResult(newFloat(rawFloatValue(asObject(self)) * rawFloatValue(asObject(arg)))));
}


static PrimitiveResult floatDivPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(floatResult(newFloat(rawFloatValue(asObject(self)) / rawFloatValue(asObject(arg)))));
}


static PrimitiveResult floatLessThanPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(asBool(rawFloatValue(asObject(self)) < rawFloatValue(asObject(arg)))));
}


static PrimitiveResult floatEqualsPrimitive(Value self, Value arg)
{
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(getTaggedPtr(asBool(rawFloatValue(asObject(self)) == rawFloatValue(asObject(arg)))));
}


static PrimitiveResult floatTruncatedPrimitive(Value self)
{
	return floatToInteger(trunc(rawFloatValue(asObject(self))));
}


static PrimitiveResult floatFloorPrimitive(Value self)
{
	return floatToInteger(floor(rawFloatValue(asObject(self))));
}


static PrimitiveResult floatCeilingPrimitive(Value self)
{
	return floatToInteger(ceil(rawFloatValue(asObject(self))));
}


static PrimitiveResult floatRoundedPrimitive(Value self)
{
	return floatToInteger(round(rawFloatValue(asObject(self))));
}


static PrimitiveResult floatSqrtPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(sqrt(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatSinPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(sin(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatCosPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(cos(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatExpPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(exp(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatLnPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(log(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatTanPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(tan(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatArcSinPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(asin(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatArcCosPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(acos(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatArcTanPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat(atan(rawFloatValue(asObject(self))))));
}


static PrimitiveResult floatArcTan2Primitive(Value self, Value arg)
{
	// self arcTan: arg  ==  atan2(self, arg); fall back to coerce a non-Float arg.
	if (!isFloatValue(arg)) {
		return primFailed();
	}
	return primSuccess(floatResult(newFloat(atan2(rawFloatValue(asObject(self)), rawFloatValue(asObject(arg))))));
}


static PrimitiveResult intAsFloatPrimitive(Value self)
{
	return primSuccess(floatResult(newFloat((double) asCInt(self))));
}


static PrimitiveResult floatAsStringPrimitive(Value self)
{
	double x = rawFloatValue(asObject(self));
	char buf[64];

	if (isnan(x)) {
		strcpy(buf, "nan");
	} else if (isinf(x)) {
		strcpy(buf, x < 0 ? "-inf" : "inf");
	} else if (x == 0.0) {
		strcpy(buf, signbit(x) ? "-0.0" : "0.0");
	} else {
		/* shortest number of significant digits that round-trips */
		int sig = 17;
		for (int s = 1; s <= 17; s++) {
			snprintf(buf, sizeof(buf), "%.*e", s - 1, x);
			if (strtod(buf, NULL) == x) {
				sig = s;
				break;
			}
		}
		/* prefer plain decimal notation for human-friendly magnitudes */
		int exp10 = (int) floor(log10(fabs(x)));
		if (exp10 >= -4 && exp10 < 16) {
			int frac = sig - 1 - exp10;
			if (frac < 0) {
				frac = 0;
			}
			snprintf(buf, sizeof(buf), "%.*f", frac, x);
			if (strtod(buf, NULL) != x) {
				snprintf(buf, sizeof(buf), "%.*e", sig - 1, x);
			}
		} else {
			snprintf(buf, sizeof(buf), "%.*e", sig - 1, x);
		}
		if (strpbrk(buf, ".eEnN") == NULL) {
			strcat(buf, ".0");
		}
	}

	HandleScope scope;
	openHandleScope(&scope);
	Value result = getTaggedPtr(asString(buf));
	closeHandleScope(&scope, NULL);
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
