#ifndef JIT_H
#define JIT_H

// Tier 1: a template compiler from the register bytecode to x86-64.
//
// "Template" means each bytecode compiles to a fixed sequence, with no register
// allocation and no reordering. That is not a stepping stone to be replaced: it
// is what makes the frame layout an INVARIANT, and the frame layout being an
// invariant is what makes deoptimization writable at all (ADR 0001).
//
// THE FRAME, and it is the whole contract:
//
//     [rbp + 8]         return address
//     [rbp + 0]         saved rbp
//     [rbp - 8]         virtual register 0   (self)
//     [rbp - 8*(i+1)]   virtual register i
//
// Slot i IS bytecode register i. No mapping, no allocator, no per-compile
// variation. A deopt map can therefore say "register 4 is live and holds a
// pointer" and mean one address.
//
// CALLING CONVENTION, and there are exactly TWO of them.
//
// NARROW, which is nearly every method: the receiver and the arguments go in
// the ABI's integer argument registers, so a compiled method is callable from C
// as an ordinary function of Values and the self-test calls one directly. Under
// SysV that is the receiver in RDI and arguments in RSI, RDX, RCX, R8, R9;
// result in RAX.
//
// WIDE, when the receiver plus the arguments do not FIT that register set:
// ONE pointer, to the CALLER's receiver slot, with the arguments at DESCENDING
// addresses from it. That is already the layout the caller has -- consecutive
// bytecode registers are consecutive frame slots and slots grow down -- and it
// is the layout the callee wants, so the prologue copies straight down instead
// of the ABI's stack-argument area being staged and then read back.
//
// `DateTime year:month:day:hour:minute:second:millisecond:` is why this exists:
// seven arguments plus a receiver is eight, SysV has six registers, and the
// prologue used to ASSERT and abort while compiling it.
//
// WHICH CONVENTION A METHOD USES IS A PROPERTY OF ITS ARITY AND ITS ABI, so
// both ends agree with nothing passed between them: the prologue decides from
// CodeUnit.argumentCount against the Abi it is emitting for, and records the
// answer in NativeCode.wide for every caller to read. Nobody derives it a
// second time, because the two derivations could disagree and the disagreement
// would be a callee reading its arguments out of registers nobody wrote.

#include "compiler/Bytecode.h"
#include "core/Object.h"
#include <setjmp.h>
#include <stddef.h>

// How many arguments the NARROW convention can carry, and it is a fact about
// THIS FILE and not about any ABI: the narrow convention is entered from C
// through positional entry points, and there are jitCall0 through jitCall5 and
// no more. An ABI with fewer argument registers simply makes more methods wide;
// an ABI with more cannot be exploited until there are entry points for it, and
// compiling a method narrow that nothing here can call would be correct code
// nobody can enter.
#define JIT_MAX_NARROW_ARGS 5

struct IcCell;
struct DeoptSite;

typedef struct NativeCode {
	CodeUnit *unit;
	// One inline-cache cell per bytecode index, populated only at send sites.
	// Indexed BY BCI rather than by a dense send number: it costs one pointer
	// per non-send instruction and buys O(1) lookup from exactly the coordinate
	// the tier, the deopt map and OSR all already carry.
	struct IcCell *cells;
	uint8_t *entry; // executable
	size_t size;
	// Byte offset into `entry` of the machine code for each bytecode index.
	// An ARRAY, not a searched table: this is the bci-to-machine map that
	// deoptimization, OSR and backtraces all read, and fixed-width bytecode is
	// what lets it be indexed rather than searched (docs/jit-v2/04-bytecode.md).
	uint32_t *machineOffsetAt;
	// What each frame slot HOLDS, which the collector needs and cannot guess
	// (memory/Roots.h, requirement R1 of ADR 0003).
	//
	// ONE MAP for a whole tier-1 method, and that is a property of THIS tier
	// rather than a simplification: the template compiler never puts a raw
	// double or a raw integer in a slot and the prologue nils every slot it does
	// not receive an argument in, so the description is the same at every point
	// in the method. Tier 2 keeps raw values in slots and its description
	// changes from safepoint to safepoint, which is what FrameMap.codeOffset is
	// for -- so the collector already asks "the map at this point" and gains
	// nothing new to learn when that day comes.
	struct FrameMap *frameMap;
	// The points this code can be LEFT at speculatively, each keyed by the code
	// offset of its guard. Empty for tier 1, which speculates about nothing.
	struct DeoptSite **deoptSites;
	uint32_t deoptSiteCount;
	uint16_t frameSlots;
	// Which of the two calling conventions the prologue emitted, decided by the
	// arity against the Abi being compiled for. Written once, here, and read by
	// every caller: see the header comment.
	_Bool wide;
	// Which code generator produced this, and whether the other one has been
	// tried on the same method.
	//
	// ON THE NativeCode and not on the CodeUnit, and that is not a preference: a
	// CodeUnit is written into the IMAGE, so a field here would be an image
	// format change for a fact that is meaningless across processes anyway --
	// loaded code has no native code at all (docs/jit-v2/01-gate.md, level 11).
	// APPENDED, because generated code reads `entry` by offsetof and every field
	// before it has to keep its place.
	_Bool optimized;
	_Bool tier2Attempted;
} NativeCode;

// Compile one unit. Returns NULL only if the unit uses an opcode this tier does
// not implement yet, and says which through `unsupported`.
NativeCode *jitCompile(CodeUnit *unit, Opcode *unsupported);
// Compile for a SPECIFIC backend. The host's output is what runs; a foreign
// backend's is what the cross-emission test inspects byte for byte on a machine
// that cannot execute it (ADR 0009).
struct MacroAssemblerOps;
NativeCode *jitCompileFor(const struct MacroAssemblerOps *ops, CodeUnit *unit,
	Opcode *unsupported);
void jitFreeNativeCode(NativeCode *code);

// Make a unit's tagged fields (its literal frame, its blocks, its selector and
// its owner class) reachable by the collector. Idempotent, and called by
// jitCompileFor, so hand-written units need nothing; what has to call it
// explicitly is whatever BUILDS a unit that will not run immediately, which is
// exactly a block's unit.
void jitRegisterUnit(CodeUnit *unit);

// Call compiled code. The variants exist because the argument registers are
// positional. Every one of them ASSERTS that the code is NARROW, because
// calling a wide method this way would put the receiver where the callee
// expects a pointer to an argument block and then dereference it.
Value jitCall0(NativeCode *code, Value receiver);
Value jitCall1(NativeCode *code, Value receiver, Value a);
Value jitCall2(NativeCode *code, Value receiver, Value a, Value b);
Value jitCall3(NativeCode *code, Value receiver, Value a, Value b, Value c);
Value jitCall4(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d);
Value jitCall5(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d,
	Value e);
// The WIDE entry. `receiverSlot` is the address of the receiver, with the
// arguments at DESCENDING addresses from it, which is exactly what a compiled
// caller's frame already holds and what a C caller has to lay out on purpose.
// There is no arity ceiling on this path and no per-arity variant of it: the
// arguments never become C arguments.
Value jitCallWide(NativeCode *code, Value *receiverSlot);

// Send a message from C, compiling the target on the way in if it has not run
// yet. `understood` says whether anything answered; a caller that wants to know
// rather than to crash passes it, because the bootstrap legitimately asks
// classes a question most of them do not answer (a class-side `initialize`).
Value jitSend(Value receiver, const char *selector, uint64_t argc,
	const Value *argv, _Bool *understood);
Value jitSendUnary(Value receiver, const char *selector, _Bool *understood);


// ---- unwinding: non-local return, on:do:, ensure: and terminate -------------
//
// ONE CHAIN, four kinds of entry, and that is the whole design. All four are
// "a point on the C stack, ordered by nesting, that leaving through has to
// notice", and keeping them apart would mean four chains that have to be
// interleaved correctly by every walk anyway.
//
// What each kind is for:
//
//   HOME     an activation a `^` inside a block can return from (ADR 0008).
//            Pushed only when the front end saw such a `^` (CodeUnit.couldBeHome);
//   HANDLER  an `on:do:` frame. The signal walks OUTWARD for the first one whose
//            class answers `handles:`, runs the handler ON TOP of the signaling
//            frames, and only then unwinds, which is what makes `resume:`
//            possible at all: at decision time nothing has been popped yet;
//   CLEANUP  an `ensure:`/`ifCurtailed:` registration. It runs when an unwind of
//            ANY other kind passes through it, innermost first, and the
//            kernel's `ensure:` runs it itself on the normal path;
//   EXIT     the BOTTOM of one fiber's Smalltalk execution: the C frame that
//            entered it, and the one place terminating that fiber can land.
//            Exactly one per fiber, pushed by whoever owns that frame -- main()
//            for the main process, the spawn trampoline for every other fiber.
//
// EXIT is on this chain rather than in a jmp_buf of its own for the reason the
// other three share one: terminating a fiber has to run every pending cleanup
// between here and the bottom, innermost first, restoring the bookkeeping of
// every frame the jump skips. That is unwindRunCleanupsTo plus unwindAnswer,
// which already exist and already do it for the other two jumping kinds. A
// second mechanism would have to be interleaved with this one anyway.
//
// The Values in here are GC ROOTS and are visited through
// rootsVisitUnwindRecords (memory/Roots.h). A handler block is held across the
// whole evaluation of the block it protects, which allocates freely, so this is
// not a technicality: without it the first collection inside a protected block
// leaves the handler pointing at a corpse.
typedef enum {
	UNWIND_HOME,
	UNWIND_HANDLER,
	UNWIND_CLEANUP,
	UNWIND_EXIT,
} UnwindKind;

typedef struct UnwindRecord {
	struct UnwindRecord *previous;
	uint8_t kind;
	// A handler that is RUNNING is skipped by the search, so an exception raised
	// inside a handler goes outward instead of back into the handler that is
	// already dealing with one.
	_Bool disabled;
	uint64_t token;       // UNWIND_HOME
	Value exceptionClass; // UNWIND_HANDLER
	Value handlerBlock;   // UNWIND_HANDLER
	Value cleanupBlock;   // UNWIND_CLEANUP
	jmp_buf destination;  // UNWIND_HOME, UNWIND_HANDLER and UNWIND_EXIT
	Value answer;
	// Thread state to put back, because the jump skips every frame in between
	// and each of them would otherwise leave its bookkeeping on these chains.
	struct CompiledFrameGuard *savedFrames;
	struct HandleScope *savedScopes;
	uint64_t savedHomeToken;
} UnwindRecord;

// Push and pop. The setjmp must be taken in the frame that will be RESUMED, so
// the caller owns the record and the buffer; these only maintain the chain.
void unwindPushHandler(UnwindRecord *record, Value exceptionClass,
	Value handlerBlock);
void unwindPushCleanup(UnwindRecord *record, Value cleanupBlock);
// Mark the bottom of this fiber's Smalltalk execution. Same contract as the two
// above: the caller owns the record and takes the setjmp in its own frame.
void unwindPushExit(UnwindRecord *record);
void unwindPop(UnwindRecord *record);
// Arrived by a jump: put back the thread state the skipped frames would have
// restored on the way out, and answer what the unwinder left.
Value unwindAnswer(UnwindRecord *record);

// Terminate the RUNNING fiber: run every pending cleanup on its own stack,
// innermost first, then jump to its exit record. Never returns.
void unwindToExit(void);

// Find a handler for `exception` and act on its decision. Answers the value the
// signal expression should have, or PRIMITIVE_FAILED when no handler matched,
// which is how `Exception>>basicSignal` falls through to its `^self
// defaultAction`. Does not return at all when the handler chose to unwind.
Value jitSignalException(Value exception);

// Executable memory. Never moved and never freed, exactly like the old exec
// space: a frame still running inside superseded code has to stay valid, and
// the cheapest way to guarantee that is to never take the memory back.
uint8_t *codeSpaceAllocate(size_t bytes);
void codeSpacePublish(uint8_t *destination, const uint8_t *bytes, size_t size);

// ---- reaching compiled frames from the collector ---------------------------
//
// A runtime helper that can ALLOCATE has to leave the collector a way back into
// the compiled frames underneath it, or a collection triggered by that
// allocation walks no compiled roots at all and evacuates objects that live
// methods are still holding.
//
// The anchor costs nothing in generated code. Two facts are already on hand:
//
//   * the FRAME POINTER, because the call passed the address of a frame slot
//     and the slot's index is known, so rbp is that address plus the offset;
//   * the METHOD, because __builtin_return_address(0) lands inside the compiled
//     caller and every compiled method's byte range is registered.
//
// A CHAIN and not a single value, and that is the part that is easy to get
// wrong. Compiled code calls the runtime, which re-enters compiled code, which
// calls the runtime again, so the compiled frames come in SEGMENTS separated by
// C frames the walk cannot cross:
//
//     churn (compiled) -> jitDispatch (C) -> new: (compiled) -> primitive (C)
//
// Anchoring only the newest segment finds `new:`'s frame and stops at the C
// boundary above `churn`, leaving churn's live registers unscanned. Every guard
// links to the one below it, so the walk resumes at the next segment instead of
// ending at the first gap. Measured: with a single anchor the outer method's
// Array was never promoted, because the collector never saw it.
typedef struct CompiledFrameGuard {
	struct CompiledFrameGuard *previous;
	uint8_t *frame;
	NativeCode *code;
	// Where inside `code` execution was when this frame called out. The frame
	// map is looked up by it, which costs nothing today (a tier-1 method has one
	// map) and is what stops the walk needing to change for tier 2.
	void *returnAddress;
} CompiledFrameGuard;

// `slotAddress` is the address the runtime call was handed, and `slotIndex` is
// which frame slot that was. `returnAddress` must be the helper's own
// __builtin_return_address(0), taken IN THE HELPER: taking it here would name
// the helper instead of the method.
void compiledFrameEnter(CompiledFrameGuard *guard, Value *slotAddress,
	uint16_t slotIndex, void *returnAddress);
void compiledFrameLeave(const CompiledFrameGuard *guard);

// Put a finished NativeCode on the list the collector and jitCodeContaining
// read. Called LAST, once every field the visitor reads is populated: a
// collection between the allocation and this call would walk a half-built
// entry. Both code generators end here, because there is one list.
void compiledCodeRegister(NativeCode *code);

// The compiled method containing `address`, or NULL when it is not inside any.
// How the walk decides whether a return address belongs to a compiled frame or
// to the C code that entered one.
NativeCode *jitCodeContaining(const void *address);

// The frame description in force at `returnAddress`, which must lie inside
// `code`. Never NULL for a method this JIT compiled.
struct FrameMap;
const struct FrameMap *jitFrameMapAt(const NativeCode *code,
	const void *returnAddress);

// Forget every RESOLVED TARGET at every send site, keeping the profile. What
// any change to a method dictionary has to do, or a warm site keeps calling the
// method that was there before. Details at the definition.
void jitFlushSendCaches(void);

// Every method this JIT has compiled, for a caller that wants to ask a question
// of all of them. Exposed for ONE reason and it is a measurement one: the tier-2
// dry run has to be able to run over methods whose caches are WARM, and a method
// is cold at the moment it compiles -- so the sweep happens later, over this.
size_t jitCompiledCount(void);
NativeCode *jitCompiledAt(size_t index);

// Run the front half of tier 2 (SsaBuild plus the passes) over a unit and throw
// the result away, under ST_TIER2_DRYRUN=1. Weakly no-op when the optimizer is
// not linked; the real one is jit/Tier2DryRun.c.
void tier2DryRun(CodeUnit *unit);

// Compile `tier1`'s method with tier 2 and answer the result, or NULL to leave
// tier 1's code standing. Off unless ST_TIER2_ALL or ST_DEOPT_STRESS asked for
// it, and weakly no-op when tier 2 is not linked.
//
// A TESTING MODE AND NOT THE TIER POLICY: it upgrades everything it can, at the
// first send that reaches the method, which is the crudest possible rule. See
// jit/Tier2Stress.c for why the timing is what it is and why a stress mode with
// no optimized code running would be a green that means nothing.
NativeCode *tier2StressUpgrade(NativeCode *tier1);

// The unconditional send path, called by compiled code. `receiverSlot` points
// at the receiver's frame slot; arguments are at DESCENDING addresses from it,
// because consecutive bytecode registers are consecutive slots and slots grow
// down.
// The send path, called by compiled code. `cell` is this site's inline cache,
// which carries the selector and accumulates the profile. `receiverSlot` points
// at the receiver's frame slot; arguments are at DESCENDING addresses from it,
// because consecutive bytecode registers are consecutive slots and slots grow
// down.
Value jitDispatch(void *cell, Value *receiverSlot, uint64_t argc);

// The `super` send path. Identical to the line above except for where the method
// search starts: at the class ABOVE the one that DEFINED the running method,
// which the compiler resolved into the cell (jit/InlineCache.h, lookupStart).
// It cannot be derived here, because the receiver may be an instance of a
// subclass and starting from its class would find the running method again.
Value jitDispatchSuper(void *cell, Value *receiverSlot, uint64_t argc);

// The rest of the runtime entry points compiled code calls. They were static
// while the template compiler was the only caller; the SSA backend is a SECOND
// code generator and needs the same ones, so they are declared rather than
// duplicated.
//
// EVERY ONE HAS THE SAME SHAPE, and the narrowness is deliberate: an object
// pointer, the ADDRESS of a frame slot, and a packed integer that says WHICH
// slot that was plus whatever else the helper needs. The slot index is what
// turns the address back into a frame pointer (compiledFrameEnter), which is how
// a collection underneath any of these finds the compiled frames beneath it.
// A second code generator that laid its frame out differently would hand these
// an index meaning something else and the reconstruction would be silently
// wrong, which is why jit/Lir.h keeps tier 1's frame law.
Value jitStoreGlobal(void *unit, Value *valueSlot, uint64_t literalIndex);
Value jitMakeClosure(void *unit, Value *baseSlot, uint64_t packed);
Value jitMakeCell(void *unused, Value *valueSlot, uint64_t packed);
Value jitSetCell(void *unused, Value *cellSlot, uint64_t packed);
Value jitReturnOuter(void *unused, Value *valueSlot, uint64_t packed);
// A barriered store into ANY tagged field, which tier 1 has no caller for: its
// OP_SETIVAR is an inline store and its cell store goes through jitSetCell. The
// SSA backend needs the general one because the SSA IR models both as
// IR_SETFIELD_T and cannot tell them apart. See the definition.
Value jitStoreField(void *unused, Value *objectSlot, uint64_t packed);

#endif
