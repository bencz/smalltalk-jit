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
// CALLING CONVENTION: the SysV integer argument registers, so a compiled method
// is callable from C as an ordinary function of Values, and the self-test calls
// one directly. Receiver in RDI, arguments in RSI, RDX, RCX, R8, R9; result in
// RAX.

#include "compiler/Bytecode.h"
#include "core/Object.h"
#include <stddef.h>

#define JIT_MAX_REGISTER_ARGS 5 // receiver plus five, the SysV integer set

struct IcCell;

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
	uint16_t frameSlots;
	// Every compiled unit, chained, so the collector can reach the heap
	// references this structure holds outside the heap: the unit's literal
	// frame and each cell's selector (memory/Roots.h, rootsVisitCompiledCode).
	struct NativeCode *nextCompiled;
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
// positional; anything wider goes through jitCallN.
Value jitCall0(NativeCode *code, Value receiver);
Value jitCall1(NativeCode *code, Value receiver, Value a);
Value jitCall2(NativeCode *code, Value receiver, Value a, Value b);
Value jitCall3(NativeCode *code, Value receiver, Value a, Value b, Value c);
Value jitCall4(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d);
Value jitCall5(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d,
	Value e);

// Send a UNARY message from C, compiling the target on the way in if it has not
// run yet. `understood` says whether anything answered; a caller that wants to
// know rather than to crash passes it, because the bootstrap legitimately asks
// classes a question most of them do not answer (a class-side `initialize`).
Value jitSendUnary(Value receiver, const char *selector, _Bool *understood);

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
} CompiledFrameGuard;

// `slotAddress` is the address the runtime call was handed, and `slotIndex` is
// which frame slot that was. `returnAddress` must be the helper's own
// __builtin_return_address(0), taken IN THE HELPER: taking it here would name
// the helper instead of the method.
void compiledFrameEnter(CompiledFrameGuard *guard, Value *slotAddress,
	uint16_t slotIndex, void *returnAddress);
void compiledFrameLeave(const CompiledFrameGuard *guard);

// The compiled method containing `address`, or NULL when it is not inside any.
// How the walk decides whether a return address belongs to a compiled frame or
// to the C code that entered one.
NativeCode *jitCodeContaining(const void *address);

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

#endif
