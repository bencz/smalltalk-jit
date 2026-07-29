#ifndef LIR_H
#define LIR_H

// The low-level IR the SSA backend allocates registers over and emits from.
//
// WHY A SECOND IR AND NOT THE MACRO ASSEMBLER. ADR 0009 puts the tier-1 seam at
// a vocabulary whose words are BYTECODE-shaped: "load frame slot i", "send".
// That vocabulary cannot say "add these two registers", because in tier 1 there
// are no registers to name -- slot i IS bytecode register i and every sequence
// begins and ends in memory. A backend with a real allocator has to talk about
// registers, so it needs words for them, and inventing those words inside the
// macro assembler would drag register allocation into the tier whose whole
// correctness argument is that it has none.
//
// WHAT IS NEUTRAL HERE, AND WHAT IS NOT.
//
// Neutral: three-address operations over VIRTUAL registers, which is what POWER
// and ARM want natively and what x86 reaches by a move plus a two-address
// instruction. The lowering never sees a machine register.
//
// Neutral by construction rather than by luck: THERE ARE NO FLAGS. A comparison
// and the branch that reads it are ONE instruction here (LIR_CMP_BRANCH), and a
// comparison whose result becomes a value is another (LIR_CMP_SET). x86 keeps
// EFLAGS, POWER has eight condition fields, ARM has NZCV, and any LIR that
// modelled one of those would be modelling exactly one of them. Fusing removes
// the question instead of answering it in a portable-looking way.
//
// NOT neutral, and deliberately: the FRAME. See below.
//
// ---------------------------------------------------------------------------
// THE FRAME, WHICH IS THE SAME LAW AS TIER 1
// ---------------------------------------------------------------------------
//
//     [rbp + 8]        return address
//     [rbp + 0]        saved frame pointer
//     [rbp - 8*(i+1)]  frame slot i
//
// Identical to jit/Jit.h. What differs is what a slot MEANS: in tier 1 slot i is
// bytecode register i, and here it is whatever the allocator put there, which is
// a spilled value or a word of the outgoing argument area.
//
// KEEPING THE LAW IS NOT TIDINESS, IT IS WHAT MAKES THE EXISTING RUNTIME WORK
// UNCHANGED. Every runtime helper compiled code calls is handed the ADDRESS of
// one frame slot plus, packed into an immediate, WHICH slot that was, and
// compiledFrameEnter turns the pair back into a frame pointer:
//
//     frame = slotAddress + 8*(slotIndex + 1)      (jit/Jit.c)
//
// That frame pointer is how a collection triggered underneath a send finds the
// compiled frames beneath it. A tier 2 that laid its frame out differently would
// hand the same helpers a slot address whose index means something else, and the
// reconstructed frame pointer would be wrong by however much the layouts
// differed. Nothing would fail: the walk would scan the wrong words, find
// plausible-looking garbage or nothing at all, and the collector would evacuate
// objects a running frame still holds. So the layout is shared on purpose, and
// the outgoing argument area is placed at LOW slot indices precisely so a send's
// receiver has an index to name.
//
// ---------------------------------------------------------------------------
// WHY EVERY VIRTUAL REGISTER CARRIES A SlotKind
// ---------------------------------------------------------------------------
//
// Requirement R1 of ADR 0003: a frame slot holds a pointer, a raw double or a
// raw integer, and the collector must never treat the last two as pointers. In
// tier 1 the answer is uniform -- the template compiler never puts a raw value
// in a slot -- so one FrameMap describes the whole method. Here it is not: the
// point of this backend is that a double lives in a slot as eight raw bytes.
//
// So the kind travels WITH the value, from the moment the lowering creates it,
// and the map at each safepoint is read off the allocation rather than guessed
// afterwards. A map derived after the fact is the over-approximation ADR 0007
// exists to remove.

#include "jit/Abi.h"
// For MaRuntimeFunction, which this IR's one call shape REUSES rather than
// redeclares. The two are the same signature by design -- tier 2 calls exactly
// the helpers tier 1 does -- and two names for one type is how the two halves of
// a system start disagreeing about it.
#include "jit/MacroAssembler.h"
#include "memory/Roots.h"
#include "core/Object.h"
#include <stdint.h>

struct CodeUnit;
struct IcCell;
struct DeoptState;
struct DeoptSite;
struct LirFunction;

// Which register file a value lives in. Separate banks are not a refinement of
// one allocator, they are two allocations that share a walk: an integer never
// competes with a double for a register, and a single pool would either model
// that with a mask on every decision or hand out an XMM register to a pointer.
typedef enum {
	LIR_BANK_INT = 0,
	LIR_BANK_FLOAT = 1,
	LIR_BANK_COUNT = 2,
} LirBank;

// A comparison, named by MEANING rather than by any machine's condition code.
// Signed and unsigned are distinguished because tagged-integer arithmetic needs
// signed and a class-index compare needs neither, and getting that wrong is a
// wrong answer on exactly the values a test is least likely to contain.
typedef enum {
	LIR_CMP_EQ,
	LIR_CMP_NE,
	LIR_CMP_LT,
	LIR_CMP_LE,
	LIR_CMP_GT,
	LIR_CMP_GE,
	LIR_CMP_ULT,   // unsigned, for address and index compares
	LIR_CMP_UGE,
} LirCondition;

typedef enum {
	// ---- moves and constants ----------------------------------------------
	LIR_MOVE,       // d := a, within one bank
	LIR_IMM,        // d := imm, a full 64-bit pattern
	// A double CONSTANT, which is an integer immediate as far as this IR is
	// concerned: the bit pattern is materialized in the integer bank and moved
	// across, because no architecture here has a 64-bit floating immediate.
	LIR_FIMM,

	// ---- memory ------------------------------------------------------------
	LIR_LOAD,       // d := [a + disp], 64 bits
	LIR_STORE,      // [a + disp] := b, 64 bits
	LIR_LOAD32,     // d := [a + disp], 32 bits, zero-extended
	LIR_STORE32,    // [a + disp] := b, low 32 bits
	// The FRAME versions, addressed by slot index in `imm` rather than by a
	// register. Separate opcodes and not the two above with the base operand
	// left out: an addressing mode inferred from whether an operand happens to
	// be absent is a distinction the reader has to reconstruct, and the
	// allocator would have to know not to give the missing operand a register.
	// The frame pointer is not a virtual register and never becomes one.
	LIR_LOAD_SLOT,  // d := frame slot imm
	LIR_STORE_SLOT, // frame slot imm := a
	// Read the 64-bit word at a FIXED address. The address is an immediate
	// because it is a slot the RUNTIME owns and therefore never moves: a
	// CodeUnit's literal field, a safepoint flag. An object's address is never
	// spelled this way, which is the rule that keeps a collector-moved pointer
	// out of the instruction stream.
	LIR_LOAD_ABS,
	// d := the ADDRESS of frame slot `imm`. How a send hands the runtime its
	// argument block, and the one place a frame slot index appears as a value.
	LIR_FRAME_ADDR,

	// ---- integer arithmetic, on RAW 64-bit values --------------------------
	LIR_ADD, LIR_SUB, LIR_MUL, LIR_DIV, LIR_MOD,
	LIR_AND, LIR_OR, LIR_XOR,
	LIR_SHL, LIR_SAR,     // b is a register
	LIR_ADDI, LIR_ANDI, LIR_SHLI, LIR_SARI,  // imm forms, which every target has
	LIR_NEG,
	// The same three, plus "the result MUST fit a SmallInteger, and leaving
	// optimized code is what happens when it does not". The operands are the
	// UNBOXED 62-bit payloads, so the range that has to hold is the tagging
	// range and not the machine's 64-bit one.
	//
	// ONE OPCODE EACH and not an add followed by a separate range test, for the
	// reason LIR_GUARD_CLASS is one: the branch structure between the check and
	// the failure is the backend's, and how a target learns that an addition
	// overflowed is the most machine-specific fact there is -- x86 has a flag,
	// and a target without one has to compute the answer. An LIR that spelled
	// the test out would be spelling out one machine's.
	LIR_GUARDED_ADD, LIR_GUARDED_SUB, LIR_GUARDED_MUL,

	// ---- floating point ----------------------------------------------------
	LIR_FADD, LIR_FSUB, LIR_FMUL, LIR_FDIV, LIR_FNEG, LIR_FSQRT,
	LIR_I2F,        // integer bank -> float bank, numeric conversion
	LIR_F2I,        // float bank -> integer bank, truncating
	// Bit REINTERPRETATION between banks, which is what boxing a double is: the
	// eight bytes do not change, only which file holds them. Distinct from I2F
	// and F2I, and confusing the two answers 1 for 1.0 or 4607182418800017408
	// for 1.0 depending on the direction.
	LIR_BITCAST_I2F, LIR_BITCAST_F2I,

	// ---- control -----------------------------------------------------------
	LIR_JUMP,       // -> succ[0]
	// Compare and branch as ONE instruction. See the header comment: this is
	// what keeps a flags register out of an IR that four architectures share.
	LIR_CMP_BRANCH,     // if (a cond b) -> succ[0] else -> succ[1]
	LIR_CMP_BRANCH_IMM, // the same against an immediate
	LIR_CMP_SET,        // d := (a cond b) ? 1 : 0
	LIR_RET,            // return a

	// ---- calls -------------------------------------------------------------
	// The ONE call shape compiled code makes, and it is narrow on purpose: a
	// general call sequence is where ABI differences (shadow space, alignment,
	// argument order) turn into one bug per backend. It matches the tier-1
	// vocabulary exactly (MaRuntimeFunction in jit/MacroAssembler.h), so the
	// helpers the runtime already exports are callable from here with nothing
	// added on their side.
	//
	//   d := function(pointerImm, &slot[slotIndex], integerImm)
	LIR_CALL_RUNTIME3,
	// Call the entry point held in register a, with the frame-slot block at
	// `imm` as the argument list. How a resolved send reaches compiled code.
	LIR_CALL_TARGET,
	// Try a primitive, returning its answer if it succeeded and falling through
	// if it failed. Folded like the tier-1 operation of the same name, and for
	// the same reason: both arms are the backend's business and the fallthrough
	// target is always the very next instruction.
	LIR_CALL_PRIMITIVE,

	// ---- everything else ---------------------------------------------------
	// A point where the execution may leave (ADR 0007). Carries the frame
	// description, which is why it is an instruction and not a marker: the
	// allocator has to see it in order to know which values are live across it.
	// A speculation. `imm` is the class index the value must have; failing it
	// LEAVES optimized code, so it carries a deoptimization site.
	//
	// ONE opcode and not the six-instruction sequence spelled out, for the same
	// reason a send is one: the branch structure between the check and the
	// failure is the backend's, and a lowering that owned it would be naming
	// machine-level facts again (ADR 0009).
	LIR_GUARD_CLASS,
	LIR_SAFEPOINT,
	// Where a method starts. Present so the prologue has a position in the
	// numbering and incoming arguments have a definition point.
	LIR_ENTRY,

	LIR_OP_COUNT
} LirOp;

// No register. Both banks number from zero, so -1 is the only value that cannot
// be mistaken for either.
#define LIR_NO_VREG ((uint32_t) 0xFFFFFFFFu)
#define LIR_NO_REG ((int16_t) -1)
#define LIR_NO_SLOT ((int32_t) -1)

// The maximum operands an instruction reads. Two covers every op above; the
// call ops read their arguments out of the frame rather than out of registers,
// which is what keeps this number small and the allocator simple.
#define LIR_MAX_ARGS 2

typedef struct LirInstruction {
	uint16_t op;
	uint8_t condition;      // LirCondition, for the compare ops
	uint8_t argCount;
	uint32_t dst;           // vreg defined, or LIR_NO_VREG
	uint32_t args[LIR_MAX_ARGS];
	// Where the allocator put each of them. Written by lirAllocateRegisters and
	// read by the emitter, which therefore never asks where a value lives: by
	// the time it runs the question has one answer per operand. LIR_NO_REG until
	// allocation has run, and on an operand that has none.
	int16_t dstReg;
	int16_t argReg[LIR_MAX_ARGS];
	int64_t imm;
	int32_t disp;
	// Position in the linear numbering, assigned once the blocks are ordered.
	// EVEN for instructions and ODD for the gaps between them, which is what
	// lets a value defined here and a value used here be distinguished without
	// a special case: a definition starts at position+1, a use ends at position.
	int32_t position;
	// LIR_CALL_RUNTIME3: the C function to call. A FUNCTION POINTER and not a
	// void*, because ISO C does not let those convert, which is the same reason
	// MaRuntimeFunction is one.
	MaRuntimeFunction function;
	// LIR_CALL_RUNTIME3: its first argument, a CodeUnit or an inline-cache cell.
	// LIR_LOAD_ABS: the fixed address to read. Never both, and never an object:
	// a runtime-owned address is what makes it legal to bake at all.
	void *pointerArg;
	const void *address;
	// The OUTGOING ARGUMENT BLOCK this call hands the runtime: which frame slots
	// it wrote, and what the collector must believe about them.
	//
	// It has to be recorded, and the reason is not bookkeeping. Between the
	// caller writing these slots and the callee's prologue reading them, the
	// runtime LOOKS THE METHOD UP, and a lookup that has to compile its target
	// allocates. So a collection can happen with an argument reachable from
	// nowhere but here: its interval ends AT the call, so the allocator is free
	// to leave it in a caller-saved register, and the register does not survive.
	// Uncovered, the object is evacuated and the callee receives a corpse.
	//
	// ONE KIND for the whole block, which every current caller satisfies: a
	// send's operands are all tagged, and the float helpers pass one raw double.
	uint16_t outgoingBase;
	uint16_t outgoingCount;
	uint8_t outgoingKind;   // SlotKind
	// The frame description in force here, built by the allocator for the ops
	// that can leave: the calls and the safepoint. NULL elsewhere.
	FrameMap *frameMap;
	// The state to resume with, carried through from the SSA instruction this
	// came from and still expressed in ITS terms -- values, not locations. The
	// allocator translates it; see DeoptSite in jit/Deopt.h for why the
	// translation cannot happen earlier or later.
	struct DeoptState *deopt;
	// And the translation, once it exists. NULL until the allocator has run,
	// and NULL forever on an instruction that carries no state.
	struct DeoptSite *deoptSite;
	// Byte offset of this instruction's first emitted byte, filled in by the
	// emitter so the frame maps can be anchored after the fact.
	uint32_t codeOffset;
	struct LirInstruction *next;
} LirInstruction;

typedef struct LirBlock {
	uint32_t id;
	// The bytecode index this block starts at, carried from the SSA block so a
	// machine offset can still be attributed to a bci for backtraces and OSR.
	uint16_t label;
	LirInstruction *first, *last;
	struct LirBlock **preds;
	uint16_t predCount, predCapacity;
	struct LirBlock *succs[2];
	uint8_t succCount;
	// Set by the ordering pass. Both are positions in the linear numbering.
	int32_t from, to;
	struct LirBlock *next;
} LirBlock;

// One live range of one interval. A vreg is live across [from, to), and the
// ranges of an interval are disjoint and ordered: the HOLES between them are
// the whole reason a range list exists rather than a single pair. A value
// defined before a loop, unused inside it and used after has a hole covering
// the loop body, and an allocator that cannot see the hole keeps a register
// pinned across code that had no use for it.
typedef struct LirRange {
	int32_t from, to;
	struct LirRange *next;
} LirRange;

// A use of an interval, with whether a register is REQUIRED there. A use that
// merely reads a value can be served from a spill slot on a target with memory
// operands; one that is the destination of an instruction cannot.
typedef struct LirUse {
	int32_t position;
	_Bool needsRegister;
	struct LirUse *next;
} LirUse;

// What the allocator assigns to. One vreg becomes one or more intervals: a
// SPLIT produces a second interval for the same vreg covering a later part of
// its life, with its own location, and a move between them where the split
// happened. That is what "linear scan with splitting" means, and it is the
// difference between spilling a value for its whole life because one loop had
// no register free and spilling it only there.
typedef struct LirInterval {
	uint32_t vreg;
	uint8_t bank;
	uint8_t kind;           // SlotKind: what the collector must believe
	int16_t reg;            // physical register, or LIR_NO_REG when spilled
	int32_t spillSlot;      // frame slot index, or LIR_NO_SLOT
	LirRange *ranges;       // ordered, disjoint
	LirUse *uses;           // ordered
	// The interval this one was split off from, and the one split off from it.
	// The chain is in position order, so the location of a vreg at a position
	// is found by walking it.
	struct LirInterval *splitParent;
	struct LirInterval *splitNext;
	// FIXED intervals model a physical register being unavailable: the ranges
	// of a caller-saved register cover every call, so the allocator refuses to
	// hand it out across one without being told about calls separately.
	_Bool fixed;
	struct LirInterval *next;
} LirInterval;

typedef struct LirFunction {
	struct CodeUnit *unit;
	// The tier-1 compilation this method already has, which deoptimization
	// resumes into and send sites share their cache cells with.
	struct NativeCode *tier1;
	const Abi *abi;
	LirBlock *entry;
	LirBlock *blocks;          // linked through LirBlock.next, in layout order
	uint32_t blockCount;
	uint32_t vregCount;
	// Parallel to the vregs. Written by the lowering, read by everything after.
	uint8_t *vregBank;
	uint8_t *vregKind;
	LirInterval *intervals;    // built by the allocator
	// Frame slots. Settled BEFORE allocation, because a call bakes the index of
	// its argument block into an immediate and that index must not depend on how
	// the allocation turned out:
	//
	//   slot 0                             self, or the running closure
	//   [1, parameterSlots)                the incoming arguments
	//   [outgoingBase, +outgoingSlots)     the outgoing argument area, reused
	//   [spillBase, frameSlots)            spill slots
	//
	// SLOT 0 IS NOT A CHOICE. jitReturnOuter and jitMakeClosure both reach the
	// running closure by reading frame[-1], which is slot 0, because in tier 1
	// register 0 of a block's frame IS the closure. A tier 2 that put something
	// else there would have those two helpers read whatever the allocator
	// happened to spill, and `^` from inside a block would follow a garbage home
	// token. So the receiver keeps slot 0 here too, and the incoming arguments
	// keep the slots after it, which additionally means the PROLOGUE IS TIER 1'S
	// UNCHANGED -- including the wide convention, which copies straight down.
	//
	// It also removes the need to model fixed argument registers in the
	// allocator at all: IR_PARAM lowers to a LOAD from its slot, so no interval
	// is ever born pinned to a physical register.
	uint16_t parameterSlots;   // 1 + argumentCount
	uint16_t outgoingBase;
	uint16_t outgoingSlots;
	uint16_t spillBase;
	// Where the guard sequence spills the register file, or 0 when the method
	// has no guard and pays nothing for one.
	uint16_t deoptSaveBase;
	uint16_t frameSlots;
	uint16_t argumentCount;
	// Which callee-saved registers the allocation used, as a BITSET per bank.
	// The prologue owes a save for each, and the epilogue a restore; written by
	// the allocator because it is the only thing that knows.
	uint32_t calleeSavedUsed[LIR_BANK_COUNT];
	// IR value id -> virtual register. Kept rather than freed with the lowering,
	// because translating a deopt state needs it and only the allocator can do
	// that translation (jit/Deopt.h).
	uint32_t *deoptVregOf;
	uint32_t deoptVregCapacity;
	// The deoptimization sites, MALLOC'd rather than arena-allocated and
	// transferred to the NativeCode when compilation succeeds. The emitter
	// bakes a site's address into the guard sequence, so a site that died with
	// the arena would be a dangling pointer inside executable memory -- the
	// same family as the baked pointers this repository has paid for four times.
	// lirDestroy frees any that were never transferred.
	struct DeoptSite **deoptSites;
	uint32_t deoptSiteCount, deoptSiteCapacity;
	_Bool deoptSitesTransferred;
	// Blocks in reverse post order, which is the order the linear numbering and
	// every dataflow walk use.
	LirBlock **order;
	uint32_t orderCount;
	// Arena, for the same reason the SSA IR has one: the LIR lives exactly as
	// long as one compilation, so nothing is ever individually freed and a
	// removed instruction can stay referenced without an ownership question.
	uint8_t *arena;
	size_t arenaUsed, arenaCapacity;
	struct LirArenaChunk *chunks;
} LirFunction;

LirFunction *lirCreate(struct CodeUnit *unit, const Abi *abi);
void lirDestroy(LirFunction *function);
void *lirAlloc(LirFunction *function, size_t bytes);

LirBlock *lirNewBlock(LirFunction *function, uint16_t label);
void lirAddEdge(LirFunction *function, LirBlock *from, LirBlock *to);
LirInstruction *lirAppend(LirFunction *function, LirBlock *block, LirOp op);
// Insert before `before`, or at the end when it is NULL. What the allocator
// uses to place a reload at a use and a spill after a definition.
LirInstruction *lirInsertBefore(LirFunction *function, LirBlock *block,
	LirInstruction *before, LirOp op);

// A fresh virtual register of a bank, and what the collector must believe about
// it. The kind is NOT derivable from the bank: a float-bank value is always
// SLOT_F64, but an integer-bank value is SLOT_POINTER when it is a tagged Value
// and SLOT_I64 when it is a raw integer, and telling those apart is the whole
// of requirement R1.
uint32_t lirNewVreg(LirFunction *function, LirBank bank, SlotKind kind);

// Order the blocks in reverse post order and assign the linear numbering. Must
// run before intervals are built; every position in the allocator refers to it.
void lirOrderAndNumber(LirFunction *function);

const char *lirOpName(LirOp op);
_Bool lirOpIsTerminator(LirOp op);
// Can the execution leave here? Calls and safepoints, which is exactly the set
// that needs a frame description.
_Bool lirOpCanLeave(LirOp op);
// Does this operation destroy registers beyond its own destination?
//
// A SUPERSET of lirOpCanLeave, and the extra members are the reason it is a
// separate question: integer DIVISION on x86 is hard-wired to a specific pair
// of registers and destroys both. Rather than teach the allocator which pair --
// which would be a machine fact inside an architecture-neutral file -- division
// declares that it clobbers the scratch set, exactly as a call does. It is
// conservative and it costs nothing today, because nothing produces an integer
// division yet; a target without the constraint pays the same nothing.
_Bool lirOpClobbers(LirOp op);
void lirPrint(const LirFunction *function);
// Free one deoptimization site and everything hanging off it.
void lirFreeDeoptSite(struct DeoptSite *site);

#endif
