// Contexts, MATERIALISED (ADR 0008).
//
// In jit-v2 an activation is a native frame and nothing else: there is no
// Context object in the heap per call. What the image sees as a Context is
// built HERE, on demand, from a frame the thread can still walk to -- the same
// walk jitPrintBacktrace does, over CurrentThread.compiledFrames.
//
// A materialised Context is an ORDINARY OBJECT of the kernel class
// MethodContext or BlockContext (packages/Core/src/Context.st), holding:
//
//   ic       where in the code the frame was, as a byte offset into the
//            native code (a SmallInteger)
//   code     the CompiledMethod or CompiledBlock object (CodeUnit.codeObject)
//   parent   nil; the parent is materialised by primContextParent on demand
//   outer    the enclosing lexical activation; for a block this kernel answers
//            its home (flat closures record no other), nil for a method
//   home     for a block, the activation of the method it was written in;
//            nil for a method
//   frame    the native frame pointer, as a SmallInteger (>> 3), 0 when the
//            context never had a frame or was built after its frame died
//   native   the NativeCode* the frame was running, as a SmallInteger (>> 3)
//
// `frame` and `native` are tagged integers rather than raw words so that the
// class needs no special shape: every slot is tagged, the collector walks it
// like any object, and ContextCopy can subclass it freely.
//
// LIVENESS IS PROVED, NEVER ASSUMED. Before any slot of `frame` is read, the
// frame is looked for in the CURRENT walk, and it counts as found only when
// the NativeCode at that position is the one recorded. A context whose frame
// has returned answers nil from every frame-reading question, which is the
// contract tests/ContextTest.st states: these used to be exactly the reads
// that crashed the old VM.
//
// Frames of OPTIMIZED code are left unread (nil answers): tier 2 does not keep
// every register in its frame slot, so reading them by the tier-1 layout would
// answer garbage with a straight face.

#include "runtime/primitives/Shared.h"
#include "jit/CompiledMethod.h"
#include "runtime/Closure.h"
#include "core/Smalltalk.h"
#include "memory/ObjectWalk.h"

typedef struct {
	OBJECT_HEADER;
	Value ic;
	Value code;
	Value parent;
	Value outer;
	Value home;
	Value frame;
	Value native;
} RawContext;
#define CONTEXT_POINTER_SLOTS 7

// One step of the backtrace walk: a frame, the code it runs, and where in that
// code it stands.
typedef struct {
	uint8_t *frame;
	NativeCode *code;
	const void *at;
} FrameStep;


static Class *contextClassNamed(const char *name)
{
	return getClass((char *) name);
}


// Is this heap object a kind of Context? The chain is walked by hand because
// the primitive cannot send isKindOf:.
static RawContext *asContext(Value receiver)
{
	if (!valueTypeOf(receiver, VALUE_POINTER)) {
		return NULL;
	}
	Class *root = contextClassNamed("Context");
	if (root == NULL) {
		return NULL;
	}
	RawClass *class = (RawClass *) classTableAt(&CurrentThread.heap->classes,
		rawObjectClassIndex(asObject(receiver)));
	while (class != NULL) {
		if (class == root->raw) {
			return (RawContext *) asObject(receiver);
		}
		class = rawClassSuperclass(class);
	}
	return NULL;
}


static Value pointerAsInt(const void *pointer)
{
	// Frames and NativeCode are 8-aligned, so the low bits carry nothing and
	// the address fits a SmallInteger with room to spare.
	return tagInt((intptr_t) ((uintptr_t) pointer >> 3));
}


static void *intAsPointer(Value value)
{
	if (!valueTypeOf(value, VALUE_INT) || asCInt(value) == 0) {
		return NULL;
	}
	return (void *) ((uintptr_t) asCInt(value) << 3);
}


// Run `each` over every compiled frame the current thread can reach, newest
// first, exactly the sequence jitPrintBacktrace prints. Answers 1 when `each`
// says stop (found what it wanted).
typedef _Bool (*FrameVisitor)(const FrameStep *step, void *ctx);

static _Bool walkFrames(FrameVisitor each, void *ctx)
{
	for (const CompiledFrameGuard *guard = CurrentThread.compiledFrames;
			guard != NULL; guard = guard->previous) {
		uint8_t *frame = guard->frame;
		NativeCode *code = guard->code;
		const void *at = guard->returnAddress;
		while (frame != NULL && code != NULL) {
			FrameStep step = { frame, code, at };
			if (each(&step, ctx)) {
				return 1;
			}
			void *returnAddress = *(void **) (frame + sizeof(Value));
			uint8_t *parent = *(uint8_t **) frame;
			code = jitCodeContaining(returnAddress);
			at = returnAddress;
			frame = parent;
		}
	}
	return 0;
}


typedef struct {
	uint8_t *frame;
	NativeCode *code;
	FrameStep found;
} FindFrame;

static _Bool findFrameStep(const FrameStep *step, void *ctx)
{
	FindFrame *find = ctx;
	if (step->frame == find->frame && step->code == find->code) {
		find->found = *step;
		return 1;
	}
	return 0;
}

// Is the context's frame still on this thread's stack, running the code the
// context recorded? The pair is what makes an address that has been REUSED by
// a later call fail the test: the new occupant runs other code.
static _Bool contextFrameLive(RawContext *context)
{
	FindFrame find;
	find.frame = intAsPointer(context->frame);
	find.code = intAsPointer(context->native);
	if (find.frame == NULL || find.code == NULL || find.code->optimized) {
		return 0;
	}
	return walkFrames(findFrameStep, &find);
}


// The frame's register `index`, which tier 1 keeps at frame - 8*(index+1)
// (jit/Jit.h). Bounds against the unit's register count, so a bad index is an
// answer (0) and not a wild read.
static _Bool frameRegister(uint8_t *frame, NativeCode *code, uint16_t index,
	Value *out)
{
	if (index >= code->frameSlots) {
		return 0;
	}
	*out = *(Value *) (frame - (size_t) (index + 1) * sizeof(Value));
	return 1;
}


// A declared temporary that is captured AND assigned lives in a CELL, and the
// register holds the cell; the value the programmer means is inside it.
static Value throughCell(Value value)
{
	if (valueTypeOf(value, VALUE_POINTER) && Handles.Cell.raw != NULL
			&& rawObjectClassIndex(asObject(value)) == classIndexOf(&Handles.Cell)) {
		return ((RawCell *) asObject(value))->value;
	}
	return value;
}


static Object *materializeStep(const FrameStep *step, _Bool fillHome);


// Find the LIVE activation of the method `homeMethod` nearest to (and
// including) the walk position, and materialise it; NULL when it has already
// returned. Nearest-first is an approximation under recursion, and the one the
// image can act on: it is the activation a backtrace would show first.
typedef struct {
	RawObject *homeMethod;
	Object *context;
} FindHome;

static _Bool findHomeStep(const FrameStep *step, void *ctx)
{
	FindHome *find = ctx;
	CodeUnit *unit = step->code->unit;
	if (unit == NULL || !valueTypeOf(unit->codeObject, VALUE_POINTER)
			|| asObject(unit->codeObject) != find->homeMethod) {
		return 0;
	}
	find->context = materializeStep(step, 0);
	return find->context != NULL;
}

// The home context of a BLOCK whose code object is `blockCode`: the activation
// of the method it was written in when that is still on the stack, or a
// FRAMELESS MethodContext naming the method when it is not. Frameless and not
// nil, because `aBlock homeContext class` is a question the image asks of a
// block whose home has long returned.
static Object *materializeHome(RawCompiledMethod *blockCode)
{
	HandleScope scope;
	openHandleScope(&scope);
	CompiledMethod *code = scopeHandle(blockCode);

	Value homeMethod = code->raw->unit != NULL
		? code->raw->unit->homeMethod : (Value) 0;
	if (valueTypeOf(homeMethod, VALUE_POINTER)) {
		FindHome find = { asObject(homeMethod), NULL };
		if (walkFrames(findHomeStep, &find)) {
			return closeHandleScope(&scope, find.context);
		}
	}

	Class *class = contextClassNamed("MethodContext");
	if (class == NULL) {
		closeHandleScope(&scope, NULL);
		return NULL;
	}
	Object *context = newObject(class, 0);
	RawContext *raw = (RawContext *) context->raw;
	for (size_t i = 0; i < CONTEXT_POINTER_SLOTS; i++) {
		(&raw->ic)[i] = tagPtr(Handles.nil.raw);
	}
	raw->ic = tagInt(0);
	raw->frame = tagInt(0);
	raw->native = tagInt(0);
	if (valueTypeOf(homeMethod, VALUE_POINTER)) {
		rawObjectStorePtr((RawObject *) context->raw,
			&((RawContext *) context->raw)->code, asObject(homeMethod));
	}
	return closeHandleScope(&scope, context);
}


// One walk position as a Context object. `fillHome` bounds the recursion: the
// home of a block context is materialised WITHOUT its own home chain (a home
// is a method context, whose home is nil anyway).
static Object *materializeStep(const FrameStep *step, _Bool fillHome)
{
	CodeUnit *unit = step->code->unit;
	Class *class = contextClassNamed(
		unit != NULL && unit->isBlock ? "BlockContext" : "MethodContext");
	if (class == NULL) {
		return NULL;
	}

	HandleScope scope;
	openHandleScope(&scope);

	// Everything read out of the frame is read BEFORE the allocation can move
	// anything, and the frame itself is C stack, which nothing moves.
	Object *context = newObject(class, 0);
	RawContext *raw = (RawContext *) context->raw;
	for (size_t i = 0; i < CONTEXT_POINTER_SLOTS; i++) {
		(&raw->ic)[i] = tagPtr(Handles.nil.raw);
	}
	raw->ic = tagInt((intptr_t) ((const uint8_t *) step->at - step->code->entry));
	raw->frame = pointerAsInt(step->frame);
	raw->native = pointerAsInt(step->code);
	if (unit != NULL && valueTypeOf(unit->codeObject, VALUE_POINTER)) {
		rawObjectStorePtr((RawObject *) context->raw,
			&((RawContext *) context->raw)->code, asObject(unit->codeObject));
	}

	if (fillHome && unit != NULL && unit->isBlock) {
		// Register 0 of a block's frame is the running closure; its method
		// object leads to the home. Read fresh each time: materialising the
		// home allocates.
		Value closureValue;
		if (frameRegister(step->frame, step->code, 0, &closureValue)
				&& valueTypeOf(closureValue, VALUE_POINTER)) {
			RawClosure *closure = (RawClosure *) asObject(closureValue);
			if (valueTypeOf(closure->method, VALUE_POINTER)) {
				Object *home = materializeHome(
					(RawCompiledMethod *) asObject(closure->method));
				if (home != NULL) {
					rawObjectStorePtr((RawObject *) context->raw,
						&((RawContext *) context->raw)->home, home->raw);
					// Flat closures record no enclosing activation besides the
					// home, so the home stands in for `outer` too.
					rawObjectStorePtr((RawObject *) context->raw,
						&((RawContext *) context->raw)->outer, home->raw);
				}
			}
		}
	}
	return closeHandleScope(&scope, context);
}


// ---------------------------------------------------------------------------
// thisContext
// ---------------------------------------------------------------------------

// The successor of `frame` in the dynamic chain: the nearest walked frame at a
// HIGHER address. Frames grow down, so the caller of a frame is above it; the
// formulation by address rather than by stepping stays a total order even
// where the chain crosses a C segment (a send that went through jitDispatch
// leaves the sender reachable only through the guard the dispatcher pushed)
// or a frame is reachable through two guards.
typedef struct {
	uint8_t *frame;
	FrameStep best;
	_Bool found;
} FindAbove;

static _Bool findAboveStep(const FrameStep *step, void *ctx)
{
	FindAbove *find = ctx;
	if (step->frame > find->frame
			&& (!find->found || step->frame < find->best.frame)) {
		find->best = *step;
		find->found = 1;
	}
	return 0; // scan everything: the nearest-above wins
}


// Object>>thisContext. The compiler rewrites the pseudo-variable into this
// send (compiler/Compile.c emitValue), so the activation to materialise is the
// SENDER's: this primitive runs in the frame of the thisContext method
// itself, and the sender is the nearest frame above it. NOT one step of the
// raw frame chain: a send that came through the dispatcher has C in between,
// and the sender is reachable only through the guard the dispatcher pushed.
Value primThisContext(Value *args, uint64_t argc)
{
	(void) argc;
	PRIMITIVE_ALLOCATES(args);

	// `args` is slot 0 of this method's frame (jit/Jit.h), so the frame
	// pointer is one word above it.
	FindAbove find = { (uint8_t *) args + sizeof(Value),
		{ NULL, NULL, NULL }, 0 };
	walkFrames(findAboveStep, &find);

	Value answer = tagPtr(Handles.nil.raw);
	if (find.found && !find.best.code->optimized) {
		Object *context = materializeStep(&find.best, 1);
		if (context != NULL) {
			answer = objectTagged(context);
		}
	}
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// ---------------------------------------------------------------------------
// Frame-reading accessors
// ---------------------------------------------------------------------------

// Context>>parent: the next frame outward, materialised through the same
// nearest-above rule the thisContext send uses.
Value primContextParent(Value *args, uint64_t argc)
{
	(void) argc;
	RawContext *context = asContext(primitiveReceiver(args));
	if (context == NULL) {
		return PRIMITIVE_FAILED;
	}
	// ANCHORED BEFORE THE LIVENESS CHECK, and not only because what follows
	// allocates: the walk starts at the newest guard, so without this frame on
	// the chain a context of a frame NEWER than the newest guard would read as
	// dead while it is running.
	PRIMITIVE_ALLOCATES(args);
	if (!contextFrameLive(context)) {
		PRIMITIVE_DONE_ALLOCATING();
		return tagPtr(Handles.nil.raw);
	}
	FindAbove find = { intAsPointer(context->frame), { NULL, NULL, NULL }, 0 };
	walkFrames(findAboveStep, &find);

	Value answer = tagPtr(Handles.nil.raw);
	if (find.found && !find.best.code->optimized) {
		Object *parent = materializeStep(&find.best, 1);
		if (parent != NULL) {
			answer = objectTagged(parent);
		}
	}
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Context>>argumentAt: index. Index 0 is the RECEIVER: register 0 for a
// method; for a block, the receiver of its home activation, because register 0
// of a block frame holds the running closure and `self` is a capture the
// closure may not even carry. Indexes 1..argumentCount are the arguments,
// which sit in those registers for both (jit/Jit.h).
Value primContextArgumentAt(Value *args, uint64_t argc)
{
	if (argc != 1 || !valueTypeOf(primitiveArgument(args, 0), VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawContext *context = asContext(primitiveReceiver(args));
	if (context == NULL) {
		return PRIMITIVE_FAILED;
	}
	// Anchored so the walk can SEE frames newer than the newest guard: nothing
	// here allocates, but liveness is proved against the walk, and this frame
	// is what connects the walk to the frames above that guard.
	PRIMITIVE_ALLOCATES(args);
	intptr_t index = asCInt(primitiveArgument(args, 0));
	Value answer = tagPtr(Handles.nil.raw);

	if (index >= 0 && contextFrameLive(context)) {
		uint8_t *frame = intAsPointer(context->frame);
		NativeCode *code = intAsPointer(context->native);
		CodeUnit *unit = code->unit;
		Value value;

		if (index == 0 && unit != NULL && unit->isBlock) {
			// A block frame's slot 0 is the running closure; `self` is the home
			// activation's receiver, and nil once the home is gone, which is
			// this question's contract too.
			RawContext *home = valueTypeOf(context->home, VALUE_POINTER)
				? asContext(context->home) : NULL;
			if (home != NULL && contextFrameLive(home)
					&& frameRegister(intAsPointer(home->frame),
						(NativeCode *) intAsPointer(home->native), 0, &value)) {
				answer = value;
			}
		} else if (index == 0
				|| (unit != NULL && index <= unit->argumentCount)) {
			if (frameRegister(frame, code, (uint16_t) index, &value)) {
				answer = value;
			}
		}
	}
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Context>>temporaryAt: index (1-based). The declared temporaries occupy the
// registers right after the arguments, in declaration order
// (compiler/Compile.c compileUnit), and one that is captured and assigned
// holds its CELL, so the read goes through it.
Value primContextTemporaryAt(Value *args, uint64_t argc)
{
	if (argc != 1 || !valueTypeOf(primitiveArgument(args, 0), VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawContext *context = asContext(primitiveReceiver(args));
	if (context == NULL) {
		return PRIMITIVE_FAILED;
	}
	// Anchored for the same reason primContextArgumentAt is.
	PRIMITIVE_ALLOCATES(args);
	intptr_t index = asCInt(primitiveArgument(args, 0));
	Value answer = tagPtr(Handles.nil.raw);

	if (index >= 1 && contextFrameLive(context)) {
		uint8_t *frame = intAsPointer(context->frame);
		NativeCode *code = intAsPointer(context->native);
		CodeUnit *unit = code->unit;
		Value value;
		if (unit != NULL && frameRegister(frame, code,
				(uint16_t) (unit->argumentCount + index), &value)) {
			answer = throughCell(value);
		}
	}
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Context>>positionDescriptor: the packed line/column word Context.st decodes.
// This tier records byte positions per instruction, not line deltas, so the
// honest answer today is 0: the decode then lands on the METHOD's own line
// (`code sourceCode line + 0`), which is where the activation is, just not
// which statement of it.
Value primContextPositionDescriptor(Value *args, uint64_t argc)
{
	(void) argc;
	RawContext *context = asContext(primitiveReceiver(args));
	if (context == NULL) {
		return PRIMITIVE_FAILED;
	}
	return tagInt(0);
}


// ---------------------------------------------------------------------------
// Contexts reached through a Block
// ---------------------------------------------------------------------------

static RawClosure *closureReceiver(Value receiver)
{
	if (!valueTypeOf(receiver, VALUE_POINTER) || Handles.Closure.raw == NULL
			|| rawObjectClassIndex(asObject(receiver))
				!= classIndexOf(&Handles.Closure)) {
		return NULL;
	}
	return (RawClosure *) asObject(receiver);
}


// Block>>homeContext: the activation of the method this block was written in,
// live when it is still on the stack, frameless when it has returned.
Value primBlockHomeContext(Value *args, uint64_t argc)
{
	(void) argc;
	RawClosure *closure = closureReceiver(primitiveReceiver(args));
	if (closure == NULL || !valueTypeOf(closure->method, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	closure = closureReceiver(primitiveReceiver(args));
	Object *home = materializeHome(
		(RawCompiledMethod *) asObject(closure->method));
	Value answer = home != NULL ? objectTagged(home) : tagPtr(Handles.nil.raw);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// Block>>outerContext: the lexically enclosing activation. A flat closure
// records no such activation (ADR 0008: captures are copied, not chained), so
// what can be answered honestly is a FRAMELESS BlockContext describing the
// block itself, its home attached. Frame-reading questions on it answer nil.
Value primBlockOuterContext(Value *args, uint64_t argc)
{
	(void) argc;
	RawClosure *closure = closureReceiver(primitiveReceiver(args));
	if (closure == NULL || !valueTypeOf(closure->method, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	Class *class = contextClassNamed("BlockContext");
	if (class == NULL) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);

	Object *context = newObject(class, 0);
	RawContext *raw = (RawContext *) context->raw;
	for (size_t i = 0; i < CONTEXT_POINTER_SLOTS; i++) {
		(&raw->ic)[i] = tagPtr(Handles.nil.raw);
	}
	raw->ic = tagInt(0);
	raw->frame = tagInt(0);
	raw->native = tagInt(0);
	closure = closureReceiver(primitiveReceiver(args));
	rawObjectStorePtr((RawObject *) context->raw,
		&((RawContext *) context->raw)->code, asObject(closure->method));

	Object *home = materializeHome(
		(RawCompiledMethod *) asObject(closure->method));
	if (home != NULL) {
		rawObjectStorePtr((RawObject *) context->raw,
			&((RawContext *) context->raw)->home, home->raw);
		rawObjectStorePtr((RawObject *) context->raw,
			&((RawContext *) context->raw)->outer, home->raw);
	}
	Value answer = objectTagged(context);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
