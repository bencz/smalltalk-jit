#ifndef IR_H
#define IR_H

// SSA intermediate representation.
//
// THE CENTRAL CHOICE: representation is part of the VALUE, not a late decision
// of the backend. Every value is TAGGED, F64, I64 or BOOL, and the conversions
// between them are ordinary instructions (IR_BOX_F, IR_UNBOX_F, ...).
//
// What that buys is not tidiness. It turns boxing elimination into GVN over
// box/unbox pairs instead of a special-purpose analysis: `unbox_f(box_f(x))`
// collapses to `x` by the same rule that collapses any other redundant pure
// computation, and no pass has to know what a float is.
//
// TWO KINDS OF TYPE KNOWLEDGE, AND THEY MUST NOT MIX.
//
//   `klass`  UNCONDITIONAL. The value is that class no matter what: it came out
//            of an allocation, a box, or a constant.
//   `known`  CONDITIONAL, held by the SSA builder and never by the value. The
//            value is that class BECAUSE A GUARD SAYS SO.
//
// Storing guard-established knowledge on the value is the trap: the redundant-
// guard pass then reads it, concludes that the very guard which established it
// is redundant, removes it, and the code proceeds to read fields off objects of
// the wrong class. The symptom is silent memory corruption, not a clean
// failure. The separation is the whole defence, so `klass` is written ONLY by
// the operations listed above.

#include "compiler/Bytecode.h"
#include "core/ClassTable.h"
#include "core/Object.h"
#include <stdint.h>

typedef enum {
	REPR_VOID = 0,
	REPR_TAGGED,
	REPR_F64,
	REPR_I64,
	REPR_BOOL,
} Repr;

typedef enum {
	// constants and inputs
	IR_CONST,      // tagged constant
	// literals[k], `extra` is the literal index. A SEPARATE OP FROM IR_CONST, and
	// the separation is not taxonomy: it was one op and that was a wrong answer.
	//
	// Two reasons, and either alone is enough. The literal INDEX has to survive
	// into the backend, and folded into IR_CONST there was nowhere to put it --
	// `extra` was already spelling which opcode produced the constant, so the
	// index was simply dropped and no backend could have emitted the right load.
	// And GVN compares `op` and `extra`: two IR_CONSTs naming different literals
	// agreed on both, so the pass MERGED them and the method then read one
	// literal everywhere it should have read several. Measured, on two literals
	// in a straight line, before this split existed.
	//
	// nil, true and false stay IR_CONST and SHOULD collapse together, which is
	// the other half of why one op could not serve both: for those, equal-looking
	// means equal.
	IR_LITERAL,
	IR_FCONST,
	IR_ICONST,
	IR_PARAM,      // incoming argument, `extra` is the register

	// representation conversions: the reason this IR exists in this shape
	IR_BOX_F, IR_BOX_I, IR_BOOL2TAG,
	IR_UNBOX_F, IR_UNBOX_I, IR_TAG2BOOL,
	IR_I2F, IR_F2I,

	// raw arithmetic
	IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FNEG, IR_FSQRT,
	IR_IADD, IR_ISUB, IR_IMUL, IR_IDIV, IR_IMOD,
	IR_FCMP, IR_ICMP, // `extra` is a Condition-like comparison kind

	// memory
	IR_FIELD_T,    // tagged instance variable read, `extra` = index
	IR_FIELD_F,    // raw f64 field of a value class
	IR_SETFIELD_T, IR_SETFIELD_F,
	// A global, reached through the literal frame's Association. `extra` is the
	// literal index, and the ADDRESS never appears in the IR: the Association is
	// a heap object the collector moves, and what does not move is the unit's own
	// field holding the literal frame, which is where the backend loads from.
	// Neither is PURE: a store to the same global, or any send that could reach
	// one, changes what a later read answers.
	IR_GLOBAL, IR_SETGLOBAL,
	IR_ALOAD, IR_ASTORE, IR_ALEN,       // generic Array of tagged
	IR_VALOAD, IR_VASTORE, IR_VALEN,    // flat value array, phase 7

	// allocation. Instructions and not sends, because escape analysis has to
	// SEE an allocation in order to erase it.
	IR_NEW,        // fixed object, `extra` = class index
	IR_NEWV,       // value-class instance, fields are the arguments
	IR_ANEW,       // Array
	IR_VNEW,       // flat value array
	// The two ADR 0008 allocations. They are separate ops rather than IR_NEW
	// with a class index because the index is a RUNTIME fact -- it comes from the
	// class table -- and the SSA builder is deliberately linkable with nothing
	// but the IR (gate levels 4 and 5 do exactly that). What a cell and a closure
	// ARE is known to the backend and to the materializer, which both already
	// depend on the runtime.
	//
	// NOT ERASED YET. Escape analysis erases IR_NEW and IR_NEWV only, so these
	// survive every pass, which is the safe direction: erasing without a correct
	// materialization recipe is a wrong answer at the first failed guard, and a
	// closure's recipe needs its home token, which the deopt state does not carry
	// yet. Modelling them at all is what lets the METHOD be built.
	IR_NEWCELL,    // one argument: the value the cell is born holding
	IR_CLOSURE,    // arguments are the captures, `extra` = index into unit blocks

	// control and calls
	IR_SEND,       // residual dispatch, carries a deopt state
	IR_GUARD_CLASS,// `extra` = class index; carries a deopt state
	IR_SAFEPOINT,  // ADR 0007, carries a deopt state
	IR_RET, IR_JUMP, IR_BRANCH,
	// `^` from inside a block, which leaves the method the block was WRITTEN in
	// (ADR 0008). A terminator like IR_RET, and a different one because where it
	// goes is decided by the closure's home token and not by this frame.
	IR_RETOUTER,
	IR_PHI,

	IR_OP_COUNT
} IrOp;

struct IrBlock;
struct IrFunction;
struct DeoptState;

typedef struct IrValue {
	uint16_t op;
	uint8_t repr;
	uint8_t flags;
	uint32_t id;
	struct IrBlock *block;
	struct IrValue **args;
	uint16_t argCount;
	uint16_t argCapacity;
	int64_t extra;      // field index, class index, comparison kind, register
	Value konst;        // IR_CONST
	double fkonst;      // IR_FCONST
	int64_t ikonst;     // IR_ICONST
	// UNCONDITIONAL class knowledge only. See the header comment: writing
	// guard-established knowledge here is the bug that removes the guard.
	uint32_t klass;
	struct DeoptState *deopt;
	// The bytecode index this came from, for the ops that have a SITE: a send's
	// inline-cache cell is indexed by it, and a safepoint or a guard resumes at
	// it. BYTECODE_NO_TARGET on everything else.
	//
	// NOT read out of the deopt state, though today it could be. The state's
	// frames are documented outermost first, so frames[0].bci is this
	// instruction's index only while there is exactly one frame; the day
	// inlining puts a caller there, every reader that took frames[0] would
	// quietly start naming the caller's site and looking up the caller's cache.
	// Site identity and resumption state are different facts.
	uint16_t bci;
	struct Materialize *recipe;   // IR_FLAG_MATERIALIZE
	struct IrValue *next;
} IrValue;

typedef struct IrBlock {
	uint32_t id;
	uint16_t label;          // the bytecode index this block starts at
	IrValue *first, *last;   // instruction list
	IrValue *phis;           // phi list, threaded through `next`
	IrValue *terminator;
	struct IrBlock **preds;
	uint16_t predCount, predCapacity;
	struct IrBlock *succs[2];
	uint8_t succCount;
	_Bool sealed;            // every predecessor is known (Braun et al.)
	_Bool filled;
	// Braun: register -> current definition in this block, and the incomplete
	// phis created while the block was still unsealed.
	IrValue **defs;
	IrValue **incompletePhis;
	struct IrBlock *next;
} IrBlock;

// One virtual frame of the deoptimization state: which method, which bytecode
// index to resume at, and what each live register holds.
//
// The resumption rule, and getting it wrong is a double call that is silent and
// awful to find: the INNERMOST frame re-executes the instruction that failed,
// while every outer frame already had its call in flight and resumes at the
// instruction AFTER the send, with the result deposited in its destination
// register.
typedef struct DeoptFrame {
	CodeUnit *unit;
	uint16_t bci;
	uint16_t destRegister;  // outer frames: where the call's result goes
	_Bool innermost;
	uint16_t slotCount;
	uint16_t *slotRegister; // which register each entry describes
	IrValue **slotValue;    // what it holds, or a materialization recipe
} DeoptFrame;

typedef struct DeoptState {
	DeoptFrame *frames;     // outermost first
	uint16_t frameCount;
} DeoptState;

// What escape analysis leaves behind when it erases an allocation: the class
// and the values that fill the fields, so the object can be rebuilt at the
// moment a guard fails. Without this, erasing an allocation is simply
// incorrect, and --deopt-stress is what proves it.
typedef struct Materialize {
	uint32_t classIndex;
	_Bool flat;
	uint16_t fieldCount;
	IrValue **fields;
} Materialize;

// This "value" is not a computation at all: it is a recipe for REBUILDING an
// object that escape analysis erased. It appears only inside deoptimization
// states, and only ever as a slot's contents.
#define IR_FLAG_MATERIALIZE 1

// A SUPER send. The whole content of the keyword is WHERE THE LOOKUP STARTS: at
// the class above the one that DEFINED the running method, which the compiler
// resolved into the site's cache cell, and not at anything the receiver names.
//
// A FLAG rather than a second opcode, and the reason is the risk of the change
// rather than taste: every pass that reasons about calls tests `op == IR_SEND`
// -- memory effects, purity, what a deopt state has to survive -- and a second
// opcode would need each of those found and updated, with a missed one silently
// treating a super send as something that cannot write memory. A flag leaves all
// of them right and is read by the backend alone.
//
// Losing it is not a slow path, it is a wrong answer: an ordinary send starts
// the lookup at the receiver's class, which for a receiver that is an instance
// of a subclass finds the RUNNING METHOD again, and recurses until the stack
// runs out. That is the exact failure lookupStart exists to prevent
// (docs/jit-v2/01-gate.md).
#define IR_FLAG_SUPER 2

typedef struct IrFunction {
	CodeUnit *unit;
	IrBlock *entry;
	IrBlock *blocks;      // linked through IrBlock.next
	uint32_t blockCount;
	uint32_t valueCounter;
	uint16_t registerCount;
	// Arena. The IR lives exactly as long as one compilation, so every node
	// comes from here and the whole graph is freed in one call. No node is ever
	// individually freed, which is why a removed instruction can stay
	// referenced by a deopt state without any ownership question.
	uint8_t *arena;
	size_t arenaUsed, arenaCapacity;
	struct IrArenaChunk *chunks;
} IrFunction;

IrFunction *irCreate(CodeUnit *unit);
void irDestroy(IrFunction *function);
void *irAlloc(IrFunction *function, size_t bytes);

IrBlock *irNewBlock(IrFunction *function, uint16_t label);
IrValue *irNewValue(IrFunction *function, IrOp op);
void irAppend(IrBlock *block, IrValue *value);
void irAddArg(IrFunction *function, IrValue *value, IrValue *arg);
void irRemove(IrValue *value);
// Insert `value` immediately before `before` in its block. Used when a pass has
// to materialize a conversion at a specific use.
void irInsertBefore(IrBlock *block, IrValue *before, IrValue *value);
// Does this operation consume raw (unboxed) operands at this position? Ops not
// listed take TAGGED values, which is what decides where a conversion has to be
// inserted after a representation change.
_Bool irOperandIsRaw(IrOp op, uint16_t index);
// Replace every use of `from` with `to`, INCLUDING uses inside deopt states and
// materialization recipes. Missing those is how an optimized-away value turns
// into a wrong answer only when a guard fails, which is the hardest possible
// place to notice it.
void irReplaceAllUses(IrFunction *function, IrValue *from, IrValue *to);

Repr irOpRepr(IrOp op);
_Bool irOpIsPure(IrOp op);
// PURE IS NOT ENOUGH, and these two exist because treating it as enough is a
// wrong-answer bug. A field read is pure in the sense the optimizer cares about
// -- it has no side effect, so a dead one may be deleted -- and it is still NOT
// interchangeable with an identical read on the other side of a store. Any pass
// that reuses or MOVES a value has to ask these as well.
_Bool irOpReadsMemory(IrOp op);
_Bool irOpWritesMemory(IrOp op);
_Bool irOpIsTerminator(IrOp op);
const char *irOpName(IrOp op);

void irPrint(IrFunction *function);

#endif
