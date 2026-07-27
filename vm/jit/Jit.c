#include "jit/Jit.h"
#include "jit/MacroAssembler.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "jit/CompiledMethod.h"
#include "jit/InlineCache.h"
#include "runtime/Dictionary.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include "core/Handle.h"
#include "memory/Heap.h"
#include "os/Os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Executable memory
// ---------------------------------------------------------------------------

static uint8_t *gCodePage;
static size_t gCodeUsed;
static size_t gCodeCapacity;

#define CODE_PAGE_BYTES (256 * 1024)


uint8_t *codeSpaceAllocate(size_t bytes)
{
	bytes = (bytes + 15) & ~(size_t) 15;
	if (gCodePage == NULL || gCodeUsed + bytes > gCodeCapacity) {
		size_t size = bytes > CODE_PAGE_BYTES ? bytes : CODE_PAGE_BYTES;
		// PORT_ME(wxorx): mapped RWX. A hardened build wants W^X, which means
		// mapping RW, publishing, then flipping to RX; the publish step below
		// is where that would go, and it is separated from allocation for
		// exactly that reason.
		gCodePage = osPageAlloc(size, 1);
		ASSERT(gCodePage != NULL);
		gCodeCapacity = size;
		gCodeUsed = 0;
	}
	uint8_t *address = gCodePage + gCodeUsed;
	gCodeUsed += bytes;
	return address;
}


void codeSpacePublish(uint8_t *destination, const uint8_t *bytes, size_t size)
{
	memcpy(destination, bytes, size);
	osFlushICache(destination, size);
}


// ---------------------------------------------------------------------------
// Compiled code as a root set
// ---------------------------------------------------------------------------
//
// A CodeUnit is a malloc'd C struct that holds tagged Values (its literal
// frame), and an IcCell holds its selector as a bare RawObject* so the dispatch
// path needs no untagging. Neither lives inside a heap object, so no other root
// provider can find them, and a young collection moves precisely the objects
// they name.
//
// What that costs when it is missing is worth writing down, because it is not
// obvious from a crash: the selector pointer keeps naming the corpse in the
// evacuated semispace, selector lookup is IDENTITY on an interned Symbol, and
// the corpse is not the interned one any more. So every send from that site
// answers doesNotUnderstand for a method that plainly exists, starting at the
// first collection and never before it.

static NativeCode *gCompiledCode;


void rootsVisitCompiledCode(RootVisitor visit, void *ctx)
{
	for (NativeCode *code = gCompiledCode; code != NULL; code = code->nextCompiled) {
		CodeUnit *unit = code->unit;
		if (valueTypeOf(unit->literals, VALUE_POINTER)) {
			visit(ctx, &unit->literals);
		}
		if (valueTypeOf(unit->blocks, VALUE_POINTER)) {
			visit(ctx, &unit->blocks);
		}
		if (valueTypeOf(unit->selector, VALUE_POINTER)) {
			visit(ctx, &unit->selector);
		}
		if (valueTypeOf(unit->ownerClass, VALUE_POINTER)) {
			visit(ctx, &unit->ownerClass);
		}
		// The cells' selectors are UNTAGGED, so they are tagged for the visit
		// and written back, exactly as the class table does for its entries.
		for (uint16_t i = 0; i < unit->instructionCount; i++) {
			if (code->cells[i].selector == NULL) {
				continue;
			}
			Value slot = tagPtr(code->cells[i].selector);
			visit(ctx, &slot);
			code->cells[i].selector = asObject(slot);
		}
	}
}


// PENDING: a CodeUnit that has been built but never COMPILED is not on this
// list, so its literal frame is unrooted until its first call. It cannot bite
// yet, because the only units that exist are made and compiled together, and
// the front end (which is what will create them in quantity) should reach the
// literal frame through a tagged field of the CompiledMethod rather than
// through a raw word into a C struct. Noted here so that decision is made on
// purpose rather than discovered.


NativeCode *jitCodeContaining(const void *address)
{
	const uint8_t *pointer = address;
	// A linear scan. Correct and, for now, cheap enough: it runs once per frame
	// per collection, not per send. A sorted array of ranges is the obvious
	// replacement once there are thousands of methods, and the interface here
	// does not change when it arrives.
	for (NativeCode *code = gCompiledCode; code != NULL; code = code->nextCompiled) {
		if (pointer >= code->entry && pointer < code->entry + code->size) {
			return code;
		}
	}
	return NULL;
}


// ---------------------------------------------------------------------------
// Compiled frames as a root set
// ---------------------------------------------------------------------------
//
// EVERY SLOT OF A TIER-1 FRAME HOLDS A TAGGED VALUE. That is not an accident
// and it is what makes this function short: the template compiler never puts a
// raw double or a raw integer in a frame slot, and the prologue nils every slot
// it does not receive an argument in. So the frame map for tier 1 is uniform --
// "slots 0 to frameSlots-1, all pointers" -- and no per-safepoint map is needed
// at this tier at all.
//
// That changes at tier 2, where the SSA backend will keep raw values in slots
// and the map has to say which is which (memory/Roots.h, requirement R1 of ADR
// 0003). The uniformity is a property of the TEMPLATE compiler, so it is stated
// here rather than assumed elsewhere.
//
// The walk goes newest to oldest along the saved-frame-pointer chain and stops
// when a return address is not inside any compiled method, which is exactly the
// boundary where C called in.

void rootsVisitNativeFrames(struct Thread *thread, RootVisitor visit, void *ctx)
{
	// Every SEGMENT of compiled frames, newest first. One pass per segment,
	// because C frames sit between them and the saved-frame-pointer chain runs
	// straight through those without any way to tell.
	for (const CompiledFrameGuard *guard = thread->compiledFrames; guard != NULL;
			guard = guard->previous) {
		uint8_t *frame = guard->frame;
		NativeCode *code = guard->code;
		while (frame != NULL && code != NULL) {
			// Slot i is at frame - 8*(i+1). The same expression the backend
			// emits, and the reason the frame layout is a declared contract
			// (jit/Jit.h).
			for (uint16_t i = 0; i < code->frameSlots; i++) {
				Value *slot = (Value *) (frame - (size_t) (i + 1) * sizeof(Value));
				if (valueTypeOf(*slot, VALUE_POINTER)) {
					visit(ctx, slot);
				}
			}
			// [frame + 8] is the return address INTO THE CALLER, and [frame + 0]
			// is the caller's frame pointer. A return address that is in no
			// compiled method IS the boundary where C called in, and ends this
			// segment.
			void *returnAddress = *(void **) (frame + sizeof(Value));
			uint8_t *parent = *(uint8_t **) frame;
			code = jitCodeContaining(returnAddress);
			frame = parent;
		}
	}
}


void compiledFrameEnter(CompiledFrameGuard *guard, Value *slotAddress,
	uint16_t slotIndex, void *returnAddress)
{
	// slotAddress is frame - 8*(index+1), so the frame pointer follows from it.
	guard->frame = (uint8_t *) slotAddress
		+ (size_t) (slotIndex + 1) * sizeof(Value);
	guard->code = jitCodeContaining(returnAddress);
	guard->previous = CurrentThread.compiledFrames;
	CurrentThread.compiledFrames = guard;
}


void compiledFrameLeave(const CompiledFrameGuard *guard)
{
	ASSERT(CurrentThread.compiledFrames == guard);
	CurrentThread.compiledFrames = guard->previous;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
//
// The whole send path, with no inline cache yet: find the receiver's class by
// INDEX, walk the superclass chain looking the selector up in each method
// dictionary, compile the method if this is its first call, and enter it.
//
// Deliberately the slow, obvious version first. The inline cache is a guard
// placed in front of this, not a replacement for it: the miss path has to land
// somewhere, and that somewhere is here.

Value jitDispatch(void *cellPointer, Value *receiverSlot, uint64_t argc);


static RawCompiledMethod *lookupMethod(RawClass *class, RawObject *selector)
{
	HandleScope scope;
	openHandleScope(&scope);
	Class *classHandle = scopeHandle(class);
	String *selectorHandle = scopeHandle(selector);

	while (classHandle->raw != NULL) {
		Value dictionaryValue = classHandle->raw->methodDictionary;
		if (valueTypeOf(dictionaryValue, VALUE_POINTER)) {
			Dictionary *dictionary = scopeHandle(asObject(dictionaryValue));
			Association *found = symbolDictAssocAt(dictionary, selectorHandle);
			if (found != NULL) {
				RawCompiledMethod *method =
					(RawCompiledMethod *) asObject(found->raw->value);
				closeHandleScope(&scope, NULL);
				return method;
			}
		}
		Value super = classHandle->raw->superClass;
		if (!valueTypeOf(super, VALUE_POINTER)) {
			break;
		}
		classHandle = scopeHandle(asObject(super));
	}
	closeHandleScope(&scope, NULL);
	return NULL;
}


Value jitDispatch(void *cellPointer, Value *receiverSlot, uint64_t packed)
{
	IcCell *cell = cellPointer;
	Value receiver = *receiverSlot;
	uint64_t argc = packed & 0xFFFFFFFFu;

	// Anchor the caller's frame before doing anything that can allocate, so a
	// collection from in here can walk the compiled frames underneath.
	CompiledFrameGuard guard;
	compiledFrameEnter(&guard, receiverSlot, (uint16_t) (packed >> 32),
		__builtin_return_address(0));

	// PROFILE FIRST, before anything can fail. The receiver's class and, when
	// there is one, the first ARGUMENT's class: that second one is what lets
	// the optimizer choose between integer and floating-point arithmetic
	// without guessing, and it is the field easiest to forget.
	uint32_t receiverClass = classIndexOfValue(receiver);
	uint32_t argumentClass = argc > 0
		? classIndexOfValue(receiverSlot[-1]) : CLASS_INDEX_INVALID;
	IcWay *way = icRecord(cell, receiverClass, argumentClass);

	RawClass *class = classOf(receiver);
	RawCompiledMethod *method = lookupMethod(class, cell->selector);
	if (method == NULL) {
		// PENDING: doesNotUnderstand. Failing loudly beats returning nil, which
		// would turn a missing method into a wrong answer somewhere else.
		fprintf(stderr, "doesNotUnderstand: ");
		printRawString((RawString *) cell->selector);
		fprintf(stderr, "\n");
		abort();
	}
	if (method->native == NULL) {
		Opcode unsupported;
		method->native = jitCompile(method->unit, &unsupported);
		if (method->native == NULL) {
			fprintf(stderr, "jit: unsupported opcode %s\n", opcodeName(unsupported));
			abort();
		}
	}
	NativeCode *code = method->native;
	if (way != NULL) {
		// Remember where this class dispatches to, so a future inline fast path
		// can call it without coming through here at all.
		way->target = code;
		icPromoteHottest(cell);
	}
	// Arguments are at DESCENDING addresses from the receiver's slot, because
	// consecutive registers are consecutive slots and slots grow down.
	Value answer;
	switch (argc) {
	case 0:
		answer = jitCall0(code, receiver);
		break;
	case 1:
		answer = jitCall1(code, receiver, receiverSlot[-1]);
		break;
	case 2:
		answer = jitCall2(code, receiver, receiverSlot[-1], receiverSlot[-2]);
		break;
	default:
		FAIL(); // PENDING: arities past two
	}
	compiledFrameLeave(&guard);
	return answer;
}


// Store into a global's Association. `valueSlot` points at the frame slot
// holding the value; `literalIndex` says which literal is the Association.
static Value jitStoreGlobal(void *unitPointer, Value *valueSlot,
	uint64_t literalIndex)
{
	CodeUnit *unit = unitPointer;
	RawArray *literals = (RawArray *) asObject(unit->literals);
	RawObject *association = asObject(literals->vars[literalIndex]);
	// Field 1 of an Association is its value; through the barrier, because the
	// Association is reachable from a namespace that has long since been
	// promoted, and what is being stored into it was very likely allocated a
	// moment ago.
	rawObjectStoreValue(association, &((Value *) association->body)[1], *valueSlot);
	return *valueSlot;
}


// ---------------------------------------------------------------------------
// The compiler
// ---------------------------------------------------------------------------

typedef struct {
	MacroAssembler *assembler;
	CodeUnit *unit;
	MaLabel **labels;   // one per bytecode index, for branch targets
	uint32_t *machineOffsetAt;
	IcCell *cells;      // one per bytecode index, populated at send sites
} JitContext;


// NOT A SINGLE REGISTER NAME BELOW THIS LINE. Everything goes through the
// macro assembler, which is the only thing that knows what a register is
// (ADR 0009). The previous version of this file spelled RAX and RCX throughout
// and was, in effect, an x86-64 compiler wearing a general name.
static _Bool emitInstruction(JitContext *jit, size_t index, Opcode *unsupported)
{
	MacroAssembler *ma = jit->assembler;
	Instruction *instruction = &jit->unit->code[index];

	switch ((Opcode) instruction->op) {
	case OP_MOVE:
		maLoadSlot(ma, instruction->b);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADI:
		// The 16-bit field is SIGNED, and sign-extending before tagging is what
		// makes a negative literal work: tagging first shifts the sign bit into
		// the payload.
		maLoadImmediate(ma, tagInt((int16_t) instruction->b));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADK:
		// Through the unit's literal field, for the same reason OP_GETGLOBAL is:
		// the Array moves, the C struct pointing at it does not, and that one
		// word is kept current by rootsVisitCompiledCode. Baking the element's
		// address would be a pointer the next collection invalidates.
		maLoadAbsolute(ma, &jit->unit->literals);
		maLoadField(ma, (uint16_t) (instruction->b + 1));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADNIL:
		maLoadImmediate(ma, tagPtr(Handles.nil.raw));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADTRUE:
		maLoadImmediate(ma, tagPtr(Handles.true_.raw));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADFALSE:
		maLoadImmediate(ma, tagPtr(Handles.false_.raw));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_GETIVAR:
		maLoadSlot(ma, instruction->b);
		maLoadField(ma, instruction->c);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_SETIVAR:
		// PENDING: the write barrier.
		maStoreField(ma, instruction->a, instruction->b, instruction->c);
		return 1;

	case OP_GETGLOBAL:
		// literals[b] is the global's Association, and its value is field 1.
		//
		// The literal frame is reached through the ADDRESS OF THE UNIT'S FIELD
		// rather than by baking the Array's address, and that is the whole point:
		// the Array moves whenever the collector feels like it, but the C struct
		// holding the reference does not, and rootsVisitCompiledCode keeps that
		// one word current. So the load is always of a live pointer, and no
		// baked address ever has to be relocated.
		maLoadAbsolute(ma, &jit->unit->literals);
		maLoadField(ma, (uint16_t) (instruction->b + 1)); // element b of the Array
		maLoadField(ma, 1);                               // the Association's value
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_SETGLOBAL:
		// Through the runtime, not inline, because the store needs the
		// generational write barrier and a global Association is old almost by
		// definition while what it is being pointed at is usually young. Globals
		// are also not a hot path: reading one is, writing one is not.
		maCallRuntime3(ma, jitStoreGlobal, jit->unit, instruction->b,
			instruction->a);
		return 1;

	case OP_JUMP:
		maJump(ma, jit->labels[instruction->a]);
		return 1;

	case OP_JUMPFALSE:
		maBranchIfImmediate(ma, instruction->a, tagPtr(Handles.false_.raw),
			MA_EQUAL, jit->labels[instruction->b]);
		return 1;

	case OP_JUMPTRUE:
		maBranchIfImmediate(ma, instruction->a, tagPtr(Handles.true_.raw),
			MA_EQUAL, jit->labels[instruction->b]);
		return 1;

	case OP_GUARDCLASS:
		maBranchIfNotClass(ma, instruction->a, (uint32_t) instruction->b,
			jit->labels[instruction->c]);
		return 1;

	case OP_SEND: {
		// Arguments live in CONSECUTIVE registers above the receiver, and slots
		// grow downward, so the ADDRESS of the receiver's slot is the whole
		// argument list. No marshalling, and nothing here knows how the target
		// passes arguments: that is the ABI's business.
		// argc in the low half, the receiver's REGISTER in the high half. The
		// register number is what lets jitDispatch turn the slot address it is
		// given back into a frame pointer, which is how a collection triggered
		// under a send finds the compiled frames beneath it. It rides in an
		// immediate that was already being materialised, so it costs nothing.
		maCallRuntime3(ma, jitDispatch, &jit->cells[index], instruction->c,
			(uint64_t) instruction->n | ((uint64_t) instruction->c << 32));
		maStoreSlot(ma, instruction->a);
		return 1;
	}

	case OP_RET:
		maEpilogue(ma, instruction->a);
		return 1;

	case OP_SAFEPOINT:
		maSafepointPoll(ma, &CurrentThread.heap->safepointRequested);
		return 1;

	default:
		*unsupported = (Opcode) instruction->op;
		return 0;
	}
}


NativeCode *jitCompile(CodeUnit *unit, Opcode *unsupported)
{
	return jitCompileFor(maHostBackend(), unit, unsupported);
}


// Compile for a SPECIFIC backend. The host case is what runs; a foreign backend
// is what the cross-emission test uses to check byte-for-byte output on a
// machine that cannot execute it.
NativeCode *jitCompileFor(const MacroAssemblerOps *ops, CodeUnit *unit,
	Opcode *unsupported)
{
	JitContext jit;
	jit.unit = unit;
	jit.assembler = maCreate(ops, unit->registerCount, unit->argumentCount);
	jit.labels = calloc(unit->instructionCount, sizeof(MaLabel *));
	jit.machineOffsetAt = calloc(unit->instructionCount, sizeof(uint32_t));
	jit.cells = calloc(unit->instructionCount, sizeof(IcCell));
	ASSERT(jit.labels != NULL && jit.machineOffsetAt != NULL && jit.cells != NULL);
	// A cache cell per send site, its selector resolved once here rather than
	// on every execution.
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		if (opcodeIsSend((Opcode) unit->code[i].op)) {
			jit.cells[i].selector = asObject(
				((RawArray *) asObject(unit->literals))->vars[unit->code[i].b]);
		}
	}
	for (size_t i = 0; i < unit->instructionCount; i++) {
		jit.labels[i] = maNewLabel(jit.assembler);
	}

	maPrologue(jit.assembler, tagPtr(Handles.nil.raw));

	// The primitive goes here, between the prologue and the first bytecode: the
	// frame is already built, so the primitive reads the receiver and arguments
	// straight out of their slots, and a failure falls through with the frame
	// exactly right for the method body that handles the general case.
	//
	// It is deliberately NOT in jitDispatch. Putting it inside the method means
	// an inline-cached call that jumps directly to this entry point still gets
	// the primitive, which is the whole point of having one.
	//
	// Note also what this does to the bci map: machineOffsetAt[0] points PAST
	// the primitive attempt, which is correct, because arriving at bci 0 through
	// deoptimization or OSR means the primitive has already failed once.
	if (unit->primitive != PRIM_NONE) {
		maCallPrimitive(jit.assembler,
			primitiveFunctionAt((PrimitiveNumber) unit->primitive),
			unit->argumentCount);
	}

	_Bool ok = 1;
	for (size_t i = 0; i < unit->instructionCount && ok; i++) {
		// Bind BEFORE emitting: a backward branch to this index must land on
		// the first byte of its code, and recording the offset here is also
		// what builds the bci-to-machine map.
		maBind(jit.assembler, jit.labels[i]);
		jit.machineOffsetAt[i] = (uint32_t) maOffset(jit.assembler);
		ok = emitInstruction(&jit, i, unsupported);
	}

	if (!ok) {
		maDestroy(jit.assembler);
		free(jit.labels);
		free(jit.machineOffsetAt);
		free(jit.cells);
		return NULL;
	}

	NativeCode *code = calloc(1, sizeof(NativeCode));
	ASSERT(code != NULL);
	code->unit = unit;
	code->frameSlots = unit->registerCount;
	code->machineOffsetAt = jit.machineOffsetAt;
	code->cells = jit.cells;
	if (ops == maHostBackend()) {
		code->entry = maPublish(jit.assembler, &code->size);
	} else {
		// Foreign target: keep the bytes for inspection and never map them
		// executable.
		size_t size;
		const uint8_t *bytes = maBytes(jit.assembler, &size);
		code->size = size;
		code->entry = malloc(size);
		memcpy(code->entry, bytes, size);
	}

	maDestroy(jit.assembler);
	free(jit.labels);

	// On the root list LAST, once every field the visitor reads is populated: a
	// collection triggered between the calloc and here would walk a half-built
	// entry.
	code->nextCompiled = gCompiledCode;
	gCompiledCode = code;
	return code;
}


void jitFreeNativeCode(NativeCode *code)
{
	for (NativeCode **link = &gCompiledCode; *link != NULL;
			link = &(*link)->nextCompiled) {
		if (*link == code) {
			*link = code->nextCompiled;
			break;
		}
	}
	// The machine code itself is NOT freed: exec memory is never reclaimed, so
	// a frame still running inside it stays valid forever.
	free(code->machineOffsetAt);
	free(code->cells);
	free(code);
}


// ---------------------------------------------------------------------------
// Calling in
// ---------------------------------------------------------------------------

typedef Value (*Entry0)(Value);
typedef Value (*Entry1)(Value, Value);
typedef Value (*Entry2)(Value, Value, Value);


Value jitCall0(NativeCode *code, Value receiver)
{
	Entry0 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	return entry(receiver);
}


Value jitCall1(NativeCode *code, Value receiver, Value a)
{
	Entry1 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	return entry(receiver, a);
}


Value jitCall2(NativeCode *code, Value receiver, Value a, Value b)
{
	Entry2 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	return entry(receiver, a, b);
}
