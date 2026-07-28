#include "jit/Jit.h"
#include "jit/MacroAssembler.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "jit/CompiledMethod.h"
#include "jit/InlineCache.h"
#include "runtime/Dictionary.h"
#include "runtime/Closure.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include "core/Handle.h"
#include "memory/Heap.h"
#include "os/Os.h"
#include <setjmp.h>
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
static CodeUnit *gRegisteredUnits;


// A unit is registered when it is BUILT, not when it is compiled, and the
// difference is a block: the front end builds a block's unit while compiling the
// enclosing method, and that unit does not run until somebody sends the closure
// `value`. Everything in between is ordinary program execution, collections
// included, and the block's literal frame is reachable from nowhere else.
void jitRegisterUnit(CodeUnit *unit)
{
	if (unit == NULL || unit->registered) {
		return;
	}
	unit->registered = 1;
	unit->nextUnit = gRegisteredUnits;
	gRegisteredUnits = unit;
}


static void visitUnitField(RootVisitor visit, void *ctx, Value *field)
{
	// A zeroed field is tagInt(0), not a pointer, so a unit registered before it
	// is fully populated is walkable at every moment in between.
	if (valueTypeOf(*field, VALUE_POINTER)) {
		visit(ctx, field);
	}
}


void rootsVisitCompiledCode(RootVisitor visit, void *ctx)
{
	for (CodeUnit *unit = gRegisteredUnits; unit != NULL; unit = unit->nextUnit) {
		visitUnitField(visit, ctx, &unit->literals);
		visitUnitField(visit, ctx, &unit->blocks);
		visitUnitField(visit, ctx, &unit->selector);
		visitUnitField(visit, ctx, &unit->ownerClass);
	}
	for (NativeCode *code = gCompiledCode; code != NULL; code = code->nextCompiled) {
		// The cells' selectors are UNTAGGED, so they are tagged for the visit
		// and written back, exactly as the class table does for its entries.
		for (uint16_t i = 0; i < code->unit->instructionCount; i++) {
			if (code->cells[i].selector == NULL) {
				continue;
			}
			Value slot = tagPtr(code->cells[i].selector);
			visit(ctx, &slot);
			code->cells[i].selector = asObject(slot);
		}
	}
}


// PENDING: nothing ever LEAVES either list. Methods are never discarded today,
// so a unit is live for as long as the process is; when redefinition arrives,
// retiring a method has to unregister its unit, and the block units it owns
// with it.


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
		RawClass *super = rawClassSuperclass(classHandle->raw);
		if (super == NULL) {
			break;
		}
		classHandle = scopeHandle((RawObject *) super);
	}
	closeHandleScope(&scope, NULL);
	return NULL;
}


// The body of both send paths. `lookupStart` is the class the method search
// begins at, and it is the ONLY thing that differs between an ordinary send and
// a super send: everything else, the profile included, is the same site
// machinery. `returnAddress` has to be taken in the function compiled code
// called, which is why it is a parameter here.
static Value dispatchFrom(IcCell *cell, Value *receiverSlot, uint64_t packed,
	RawClass *lookupStart, void *returnAddress)
{
	Value receiver = *receiverSlot;
	uint64_t argc = packed & 0xFFFFFFFFu;

	// Anchor the caller's frame before doing anything that can allocate, so a
	// collection from in here can walk the compiled frames underneath.
	CompiledFrameGuard guard;
	compiledFrameEnter(&guard, receiverSlot, (uint16_t) (packed >> 32),
		returnAddress);

	// PROFILE FIRST, before anything can fail. The receiver's class and, when
	// there is one, the first ARGUMENT's class: that second one is what lets
	// the optimizer choose between integer and floating-point arithmetic
	// without guessing, and it is the field easiest to forget.
	uint32_t receiverClass = classIndexOfValue(receiver);
	uint32_t argumentClass = argc > 0
		? classIndexOfValue(receiverSlot[-1]) : CLASS_INDEX_INVALID;
	IcWay *way = icRecord(cell, receiverClass, argumentClass);

	RawCompiledMethod *method = lookupStart == NULL
		? NULL : lookupMethod(lookupStart, cell->selector);
	if (method == NULL) {
		// PENDING: doesNotUnderstand. Failing loudly beats returning nil, which
		// would turn a missing method into a wrong answer somewhere else.
		// EVERY PART OF THIS GOES TO STDERR, and the selector is why: printing it
		// on stdout put it in a block-buffered stream that abort() never flushes,
		// so the message arrived as "doesNotUnderstand: " with nothing after it.
		//
		// The RECEIVER'S CLASS is named too. Without it the message says what was
		// not understood and leaves out who did not understand it, which is the
		// half that says where to look.
		fprintf(stderr, "doesNotUnderstand: ");
		fprintRawString(stderr, (RawString *) cell->selector);
		RawClass *receiverClassObject = classOf(receiver);
		Value className = receiverClassObject == NULL
			? 0 : receiverClassObject->name;
		fprintf(stderr, " (receiver is ");
		if (valueTypeOf(className, VALUE_POINTER)) {
			fprintf(stderr, "a ");
			fprintRawString(stderr, (RawString *) asObject(className));
		} else {
			fprintf(stderr, "of class %u", receiverClass);
		}
		fprintf(stderr, ")");
		// Arguments that are Symbols or Strings are printed, and nothing else is.
		// A bodyless primitive method fails as `self primitiveFailed: #TheName`
		// (compiler/Compile.c), so without this the message names the receiver's
		// class and omits WHICH primitive was being attempted, which for a class
		// like Float is a dozen candidates. Text-shaped arguments are the ones
		// worth printing and the only ones that are safe to print from here.
		for (uint64_t i = 0; i < argc; i++) {
			Value argument = receiverSlot[-(intptr_t) (i + 1)];
			if (!valueTypeOf(argument, VALUE_POINTER)) {
				continue;
			}
			RawClass *argumentClassObject = classOf(argument);
			if (argumentClassObject != Handles.String.raw
					&& argumentClassObject != Handles.Symbol.raw) {
				continue;
			}
			fprintf(stderr, " (argument %llu is ", (unsigned long long) (i + 1));
			fprintRawString(stderr, (RawString *) asObject(argument));
			fprintf(stderr, ")");
		}
		fprintf(stderr, "\n");
		fflush(NULL);
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
	//
	// Re-read from the FRAME and not from the local above: the frame slot is a
	// root the collector updates, and a bare Value held in a C local across the
	// lookup would not be. Nothing between here and there collects today, and
	// this is what keeps that from being a requirement.
	receiver = *receiverSlot;
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
	case 3:
		answer = jitCall3(code, receiver, receiverSlot[-1], receiverSlot[-2],
			receiverSlot[-3]);
		break;
	case 4:
		answer = jitCall4(code, receiver, receiverSlot[-1], receiverSlot[-2],
			receiverSlot[-3], receiverSlot[-4]);
		break;
	case 5:
		answer = jitCall5(code, receiver, receiverSlot[-1], receiverSlot[-2],
			receiverSlot[-3], receiverSlot[-4], receiverSlot[-5]);
		break;
	default:
		// Past the ABI's register set. Saying so beats calling with arguments the
		// callee will read out of registers nobody wrote.
		fprintf(stderr, "jit: a send of ");
		fprintRawString(stderr, (RawString *) cell->selector);
		fprintf(stderr, " has %llu arguments, and this tier passes at most %d\n",
			(unsigned long long) argc, JIT_MAX_REGISTER_ARGS);
		fflush(NULL);
		abort();
	}
	compiledFrameLeave(&guard);
	return answer;
}


Value jitDispatch(void *cellPointer, Value *receiverSlot, uint64_t packed)
{
	return dispatchFrom(cellPointer, receiverSlot, packed, classOf(*receiverSlot),
		__builtin_return_address(0));
}


// `super`, and the whole content of the keyword is the one argument that differs
// from the line above: the search starts where the COMPILER said, at the class
// ABOVE the one that defined the running method, and not at anything the
// receiver names. That is what makes `super foo` inside `Foo>>foo` finite, and
// it is why the start cannot be derived at runtime from the receiver, which may
// be an instance of a subclass three levels down.
//
// The receiver is unchanged: super is not another object, it is another place to
// start looking.
Value jitDispatchSuper(void *cellPointer, Value *receiverSlot, uint64_t packed)
{
	IcCell *cell = cellPointer;
	// A super send in a class with no superclass has nowhere to look, and NULL
	// takes it to doesNotUnderstand. Falling back to the receiver's class would
	// find the running method again and recurse until the stack ran out.
	RawClass *start = cell->lookupStart == CLASS_INDEX_INVALID ? NULL
		: (RawClass *) classTableAt(&CurrentThread.heap->classes, cell->lookupStart);
	return dispatchFrom(cell, receiverSlot, packed, start,
		__builtin_return_address(0));
}


// A runtime helper is handed the ADDRESS of one frame slot plus, packed into an
// immediate the call sequence was already materialising, WHICH slot that was and
// whatever else it needs. The slot index is what turns the address back into a
// frame pointer, which is how the helper reaches the rest of the frame and how a
// collection under it finds the compiled frames beneath.
#define PACKED_LOW(packed) ((uint16_t) ((packed) & 0xFFFF))
#define PACKED_MID(packed) ((uint16_t) (((packed) >> 16) & 0xFFFF))
#define PACKED_SLOT(packed) ((uint16_t) (((packed) >> 32) & 0xFFFF))


// Send a unary message from C.
//
// It exists for the bootstrap: a class-side `initialize` has to run before
// anything the class touches works, and until an image exists there is no
// Smalltalk-level place to run it from. It goes through the same lookup and the
// same entry as every other send, so a method reached this way is compiled,
// cached and profiled exactly like one reached from bytecode.
Value jitSendUnary(Value receiver, const char *selector, _Bool *understood)
{
	HandleScope scope;
	openHandleScope(&scope);
	Object *held = valueTypeOf(receiver, VALUE_POINTER)
		? (Object *) scopeHandle(asObject(receiver)) : NULL;
	String *name = asSymbol(stringFromC(selector));

	RawCompiledMethod *method = lookupMethod(classOf(
		held != NULL ? tagPtr(held->raw) : receiver), (RawObject *) name->raw);
	if (method == NULL) {
		if (understood != NULL) {
			*understood = 0;
		}
		closeHandleScope(&scope, NULL);
		return tagPtr(Handles.nil.raw);
	}
	if (understood != NULL) {
		*understood = 1;
	}
	if (method->native == NULL) {
		Opcode unsupported;
		method->native = jitCompile(method->unit, &unsupported);
		if (method->native == NULL) {
			fprintf(stderr, "jit: %s uses %s, which the tier does not implement\n",
				selector, opcodeName(unsupported));
			fflush(NULL);
			closeHandleScope(&scope, NULL);
			return tagPtr(Handles.nil.raw);
		}
	}
	Value answer = jitCall0(method->native,
		held != NULL ? tagPtr(held->raw) : receiver);
	closeHandleScope(&scope, NULL);
	return answer;
}


// ---------------------------------------------------------------------------
// Non-local return (ADR 0008)
// ---------------------------------------------------------------------------
//
// `^` inside a block returns from the method the block was WRITTEN in, however
// many activations are stacked in between. Between the block and its home there
// are compiled frames AND C frames, alternating, and the C frames are what rules
// out simply popping compiled frames.
//
// The mechanism, and what each half buys:
//
//   THE TOKEN answers "which activation", and it is a counter rather than a
//   frame address because addresses are reused: a block that outlives its home
//   must find NOTHING, and a stale address would eventually find a stranger's
//   frame sitting in the same place. The closure carries the token of the
//   activation it was born in.
//
//   THE RECORD answers "how to get there". It lives on the C frame that ENTERED
//   the home activation, so the chain unwinds itself as those frames return, and
//   it holds the jump destination plus the thread state a jump would otherwise
//   leave behind.
//
// WHO PAYS. Only a method that actually contains a `^` inside a block gets a
// record, and the front end decides that at compile time (CodeUnit.couldBeHome).
// A program that never writes one runs exactly as it did before: no token is
// minted, no record is pushed, and nothing is emitted at any send. That is why
// the cost sits here and not as a check after every send, which is the other way
// to build this and would tax the hottest path in the system for a feature most
// sends never use.

typedef struct UnwindRecord {
	struct UnwindRecord *previous;
	uint64_t token;
	jmp_buf destination;
	Value answer;
	// Thread state to put back, because the jump skips every frame in between
	// and each of them would otherwise leave its bookkeeping on these chains.
	struct CompiledFrameGuard *savedFrames;
	struct HandleScope *savedScopes;
	uint64_t savedHomeToken;
} UnwindRecord;


// Make the activation about to be entered findable by a non-local return.
static void unwindPush(UnwindRecord *record)
{
	record->token = ++CurrentThread.nextHomeToken;
	record->answer = tagPtr(Handles.nil.raw);
	record->savedFrames = CurrentThread.compiledFrames;
	record->savedScopes = CurrentThread.handleScopes;
	record->savedHomeToken = CurrentThread.homeToken;
	record->previous = CurrentThread.unwinds;
	CurrentThread.unwinds = record;
	CurrentThread.homeToken = record->token;
}


static void unwindPop(UnwindRecord *record)
{
	ASSERT(CurrentThread.unwinds == record);
	CurrentThread.unwinds = record->previous;
	CurrentThread.homeToken = record->savedHomeToken;
}


// Arrived by a jump rather than by a return: the frames in between were skipped,
// so their entries on these chains are put back from what was saved.
//
// PENDING: the handle scopes of the skipped frames are dropped by restoring the
// head, which leaks their overflow chunks. Bounded by one unwind and worth
// fixing when unwinding gets its second client, which is `ensure:`.
static Value unwindAnswer(UnwindRecord *record)
{
	CurrentThread.unwinds = record->previous;
	CurrentThread.compiledFrames = record->savedFrames;
	CurrentThread.handleScopes = record->savedScopes;
	CurrentThread.homeToken = record->savedHomeToken;
	return record->answer;
}


// RETOUTER: return `*valueSlot` from the home of the closure in slot 0. Never
// returns to its caller.
static Value jitReturnOuter(void *unused, Value *valueSlot, uint64_t packed)
{
	(void) unused;
	// The value's register is what turns its slot address back into the frame
	// pointer, and slot 0 of a block's frame is the running closure.
	uint8_t *frame = (uint8_t *) valueSlot
		+ (size_t) (PACKED_SLOT(packed) + 1) * sizeof(Value);
	RawClosure *closure = (RawClosure *) asObject(((Value *) frame)[-1]);
	uint64_t token = (uint64_t) asCInt(closure->homeToken);

	for (UnwindRecord *record = CurrentThread.unwinds; record != NULL;
			record = record->previous) {
		if (record->token != token) {
			continue;
		}
		record->answer = *valueSlot;
		longjmp(record->destination, 1);
	}
	// The home activation has already returned. ADR 0008 says this RAISES rather
	// than jumping anywhere, and the token scheme is what makes it detectable at
	// all: no live record can ever carry a retired token.
	//
	// PENDING: this is BlockCannotReturn once exceptions exist in v2. Aborting is
	// the same thing doesNotUnderstand does today, and for the same reason:
	// answering something would turn a broken program into a wrong answer.
	fprintf(stderr, "non-local return from a method that has already returned\n");
	abort();
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
// Closures (ADR 0008)
// ---------------------------------------------------------------------------
//
// All three of these ALLOCATE, so all three anchor the calling frame. They take
// the frame slot they were handed plus, in the packed integer, WHICH slot that
// was, which is what turns the address back into a frame pointer.
//
// The packing is two or three 16-bit fields in an immediate the call sequence
// was already materialising, so carrying them costs nothing.



// Build a closure over `blocks[index]`, capturing the values in the `count`
// registers starting at the base. Consecutive registers are consecutive slots
// at DESCENDING addresses, so capture i is baseSlot[-i].
static Value jitMakeClosure(void *unitPointer, Value *baseSlot, uint64_t packed)
{
	CompiledFrameGuard guard;
	compiledFrameEnter(&guard, baseSlot, PACKED_SLOT(packed),
		__builtin_return_address(0));

	CodeUnit *unit = unitPointer;
	uint16_t index = PACKED_LOW(packed);
	uint16_t count = PACKED_MID(packed);

	// WHERE the home token comes from, and it is not a detail: a `^` inside this
	// block returns from the method the block was WRITTEN in, so a block built
	// inside another block inherits that block's home rather than naming the
	// activation it happens to be running under. Taking the thread's current
	// token here would name whatever method called `value`, which is a different
	// method entirely and usually somebody else's.
	uint64_t homeToken;
	if (unit->isBlock) {
		// Slot 0 of a block's frame IS the running closure, and the frame pointer
		// is one slot above it.
		RawClosure *running = (RawClosure *) asObject(((Value *) guard.frame)[-1]);
		homeToken = (uint64_t) asCInt(running->homeToken);
	} else {
		homeToken = CurrentThread.homeToken;
	}

	HandleScope scope;
	openHandleScope(&scope);
	RawArray *blocks = (RawArray *) asObject(unit->blocks);
	Object *method = scopeHandle(asObject(blocks->vars[index]));
	Closure *closure = newClosure(method, count, homeToken);
	// The captures are copied AFTER the allocation and read from the frame each
	// time, because allocating the closure may have moved every one of them.
	for (uint16_t i = 0; i < count; i++) {
		rawObjectStoreValue((RawObject *) closure->raw, &closure->raw->captured[i],
			baseSlot[-(intptr_t) i]);
	}
	Value answer = objectTagged(closure);
	closeHandleScope(&scope, NULL);

	compiledFrameLeave(&guard);
	return answer;
}


static Value jitMakeCell(void *unused, Value *valueSlot, uint64_t packed)
{
	(void) unused;
	CompiledFrameGuard guard;
	compiledFrameEnter(&guard, valueSlot, PACKED_SLOT(packed),
		__builtin_return_address(0));
	Value answer = objectTagged(newCell(*valueSlot));
	compiledFrameLeave(&guard);
	return answer;
}


// Store into a cell, through the write barrier. A cell outlives the frame that
// made it (that is the whole point of having one), so it is routinely old while
// what goes into it is young.
static Value jitSetCell(void *unused, Value *cellSlot, uint64_t packed)
{
	(void) unused;
	uint16_t cellRegister = PACKED_SLOT(packed);
	uint16_t valueRegister = PACKED_LOW(packed);
	// Slot i sits at frame - 8*(i+1), so a higher register is a LOWER address.
	Value *valueSlot = cellSlot
		- ((intptr_t) valueRegister - (intptr_t) cellRegister);
	RawObject *cell = asObject(*cellSlot);
	rawObjectStoreValue(cell, &((RawCell *) cell)->value, *valueSlot);
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

	case OP_SEND:
	case OP_SENDSUPER: {
		// Arguments live in CONSECUTIVE registers above the receiver, and slots
		// grow downward, so the ADDRESS of the receiver's slot is the whole
		// argument list. No marshalling, and nothing here knows how the target
		// passes arguments: that is the ABI's business.
		// argc in the low half, the receiver's REGISTER in the high half. The
		// register number is what lets jitDispatch turn the slot address it is
		// given back into a frame pointer, which is how a collection triggered
		// under a send finds the compiled frames beneath it. It rides in an
		// immediate that was already being materialised, so it costs nothing.
		//
		// A super send is the SAME sequence to a different runtime entry point,
		// so where the lookup starts costs nothing here and nothing at the site:
		// it was decided when this method was compiled and it lives in the cell.
		maCallRuntime3(ma,
			(Opcode) instruction->op == OP_SEND ? jitDispatch : jitDispatchSuper,
			&jit->cells[index], instruction->c,
			(uint64_t) instruction->n | ((uint64_t) instruction->c << 32));
		maStoreSlot(ma, instruction->a);
		return 1;
	}

	case OP_RET:
		maEpilogue(ma, instruction->a);
		return 1;

	case OP_RETOUTER:
		// Never comes back, so nothing follows it and no result is stored. The
		// register rides in the immediate for the same reason every other helper
		// takes it: it is what turns the slot address into the frame pointer,
		// and from there into the running closure in slot 0.
		maCallRuntime3(ma, jitReturnOuter, NULL, instruction->a,
			(uint64_t) instruction->a << 32);
		return 1;

	case OP_CLOSURE:
		// blocks[b], capturing the n registers starting at c. The capture list
		// was gathered into consecutive registers by the front end, for the same
		// reason a send's arguments are: one address describes the whole run.
		maCallRuntime3(ma, jitMakeClosure, jit->unit, instruction->c,
			(uint64_t) instruction->b | ((uint64_t) instruction->n << 16)
			| ((uint64_t) instruction->c << 32));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_GETUP:
		// Register 0 of a block's frame IS the closure, so a captured value is
		// one load at a known offset. This is what replaces the old VM's walk up
		// a chain of contexts (ADR 0008).
		maLoadSlot(ma, 0);
		maLoadField(ma, CLOSURE_CAPTURE_FIELD(instruction->b));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_NEWCELL:
		maCallRuntime3(ma, jitMakeCell, NULL, instruction->b,
			(uint64_t) instruction->b << 32);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_GETCELL:
		maLoadSlot(ma, instruction->b);
		maLoadField(ma, CELL_VALUE_FIELD);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_SETCELL:
		// Through the runtime because the store needs the generational write
		// barrier, and a cell is exactly the object that outlives its frame.
		maCallRuntime3(ma, jitSetCell, NULL, instruction->a,
			(uint64_t) instruction->b | ((uint64_t) instruction->a << 32));
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
// The three singletons whose addresses generated code BAKES: nil in the
// prologue's slot fill and in OP_LOADNIL, true and false in OP_LOADTRUE /
// OP_LOADFALSE and in every inlined conditional's compare.
//
// Baking is legal ONLY because they never move (memory/Heap.h,
// allocateImmortalObject). This is asserted at EVERY compilation rather than
// trusted to a bootstrap, because the failure is silent: everything works until
// the first collection moves the singleton, after which a value that IS false
// stops matching the baked false and the method falls into its mustBeBoolean
// path. Measured, not imagined -- it is how this assertion came to exist.
static void assertSingletonsAreImmortal(void)
{
	ASSERT(Handles.nil.raw != NULL && isOldObject(Handles.nil.raw));
	ASSERT(Handles.true_.raw != NULL && isOldObject(Handles.true_.raw));
	ASSERT(Handles.false_.raw != NULL && isOldObject(Handles.false_.raw));
}


NativeCode *jitCompileFor(const MacroAssemblerOps *ops, CodeUnit *unit,
	Opcode *unsupported)
{
	assertSingletonsAreImmortal();
	// Idempotent: a unit built by the front end registered itself when it was
	// built, and one written by hand in a test registers here.
	jitRegisterUnit(unit);
	JitContext jit;
	jit.unit = unit;
	jit.assembler = maCreate(ops, unit->registerCount, unit->argumentCount);
	jit.labels = calloc(unit->instructionCount, sizeof(MaLabel *));
	jit.machineOffsetAt = calloc(unit->instructionCount, sizeof(uint32_t));
	jit.cells = calloc(unit->instructionCount, sizeof(IcCell));
	ASSERT(jit.labels != NULL && jit.machineOffsetAt != NULL && jit.cells != NULL);
	// A cache cell per send site, its selector resolved once here rather than
	// on every execution.
	//
	// And, at a SUPER site, where the lookup starts: the class above the one that
	// DEFINED this method, as an index, which is stable and needs no relocation
	// (ADR 0005). It is resolved here because it is a property of the site and of
	// the compile, and deriving it at runtime is impossible: the receiver may be
	// an instance of a subclass, and starting from its class would find this very
	// method again.
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		if (!opcodeIsSend((Opcode) unit->code[i].op)) {
			continue;
		}
		jit.cells[i].selector = asObject(
			((RawArray *) asObject(unit->literals))->vars[unit->code[i].b]);
		jit.cells[i].lookupStart = CLASS_INDEX_INVALID;
		if ((Opcode) unit->code[i].op != OP_SENDSUPER
				|| !valueTypeOf(unit->ownerClass, VALUE_POINTER)) {
			continue;
		}
		RawClass *super = rawClassSuperclass((RawClass *) asObject(unit->ownerClass));
		if (super != NULL) {
			jit.cells[i].lookupStart = super->classIndex;
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
	//
	// A DECLARED but not yet IMPLEMENTED primitive emits nothing at all, and the
	// method runs its Smalltalk fallback. That is what lets the whole kernel
	// compile while the VM implements the primitives a few at a time: 173 names
	// are declared in packages/ and only some have C behind them
	// (runtime/Primitives.def).
	PrimitiveFunction primitive = unit->primitive != PRIM_NONE
		? primitiveFunctionAt((PrimitiveNumber) unit->primitive) : NULL;
	if (primitive != NULL) {
		maCallPrimitive(jit.assembler, primitive, unit->argumentCount);
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


// ---------------------------------------------------------------------------
// Calling in
// ---------------------------------------------------------------------------

typedef Value (*Entry0)(Value);
typedef Value (*Entry1)(Value, Value);
typedef Value (*Entry2)(Value, Value, Value);
typedef Value (*Entry3)(Value, Value, Value, Value);
typedef Value (*Entry4)(Value, Value, Value, Value, Value);
typedef Value (*Entry5)(Value, Value, Value, Value, Value, Value);


// Entering compiled code, and the ONE place a non-local return can land.
//
// The record has to be made in THIS frame rather than inside a helper, because a
// jump resumes in the frame that took the destination: taking it one call deeper
// would resume in a frame that no longer exists. That is what the macro is for,
// and why it expands into each entry point instead of being a function of its
// own.
//
// The test is a field of the unit the compiler already filled, so a method with
// no `^` inside a block enters exactly as it did before.
#define ENTER_COMPILED(call) \
	if (!code->unit->couldBeHome) { \
		return (call); \
	} \
	UnwindRecord record; \
	unwindPush(&record); \
	if (setjmp(record.destination) != 0) { \
		return unwindAnswer(&record); \
	} \
	Value answer = (call); \
	unwindPop(&record); \
	return answer;


Value jitCall0(NativeCode *code, Value receiver)
{
	Entry0 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver))
}


Value jitCall1(NativeCode *code, Value receiver, Value a)
{
	Entry1 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a))
}


Value jitCall2(NativeCode *code, Value receiver, Value a, Value b)
{
	Entry2 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b))
}


// Three, four and five arguments. The ceiling is the ABI's: the receiver plus
// five is the SysV integer argument set (JIT_MAX_REGISTER_ARGS), and a wider
// send needs its arguments in memory, which is a calling convention this tier
// does not have yet and says so about rather than guesses at.
Value jitCall3(NativeCode *code, Value receiver, Value a, Value b, Value c)
{
	Entry3 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b, c))
}


Value jitCall4(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d)
{
	Entry4 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b, c, d))
}


Value jitCall5(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d,
	Value e)
{
	Entry5 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b, c, d, e))
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


