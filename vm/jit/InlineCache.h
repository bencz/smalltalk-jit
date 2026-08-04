#ifndef INLINE_CACHE_H
#define INLINE_CACHE_H

// Per-send-site inline caches, which are simultaneously the dispatch
// accelerator and the TYPE PROFILE the optimizer learns from.
//
// The profile is the part that matters for this project. Three things it
// records that the previous VM's caches did not:
//
//   * A COUNT PER CLASS, not merely presence. "Which class does this site see"
//     is a different question from "which class does it see NINETY PERCENT of
//     the time", and only the second justifies speculating.
//   * THE CLASS OF THE FIRST ARGUMENT, not only of the receiver. This is the
//     one that is easy to forget and it is what lets the optimizer decide
//     between integer and floating-point arithmetic without guessing: `a + b`
//     needs to know about `b`.
//   * IT SURVIVES COLLECTION. A way holds a class INDEX, not an address, so it
//     does not die with the GC epoch. The previous VM wiped every cache at
//     every scavenge because the cells held pointers, and its own notes record
//     the consequence: a long run reset each cell many times over, so the
//     profile was permanently amnesiac. That is the fourth thing the 22-bit
//     class index of ADR 0005 bought.

#include "core/Object.h"
#include <stdint.h>

struct NativeCode;

#define IC_MAX_WAYS 6

// How many ways the EMITTED cache tests before giving up and calling the
// runtime. The backend emits this many compares and icPromoteHottest orders
// exactly this many positions, so it is one constant and not two that have to
// agree.
//
// TWO, and the number came from measuring rather than from taste. With one way,
// 5.4% of Richards' sends missed and 9.5% of MixedArith's, and reading the cells
// afterwards said where those went: 100% of them to the site's SECOND class in
// both, and zero to a third. Mixed arithmetic is the clearest case -- `a + b`
// alternating between SmallInteger and Float is two classes at one site by
// construction, which is exactly the shape a one-way cache cannot hold and the
// shape ADR 0006 says the profile exists to see.
//
// Raising it further is a code-size decision that needs its own measurement:
// each way is about five more instructions at EVERY send site, paid by the many
// monomorphic sites to help the few polymorphic ones.
#define IC_EMITTED_WAYS 2

// How many executions of a WAY still pay to record the argument's class.
//
// Deriving a class index costs eight or nine instructions and it ran on every
// execution of every binary send in the system, to maintain a field whose only
// consumer (jit/Specialize.c) refuses to look below SPECIALIZE_MIN_SENDS
// executions and asks one question of it: does this argument have a
// representation worth specializing on. A few dozen samples answer that as well
// as a few million do.
//
// 64 rather than 8: the floor Specialize.c applies is on the SITE's total, and a
// polymorphic site reaches it long before any single way has been through the
// argument enough times to have seen more than one shape.
//
// The cost of the bound is written where it is paid, in the emitted sequence in
// jit/x64/MacroAssemblerX64.c: past it the field means "seen while young"
// instead of "last seen".
#define ARGUMENT_PROFILE_SENDS 64

typedef struct {
	uint32_t classIndex;    // receiver class this way matches
	// The class of the first argument, as seen during this way's first
	// ARGUMENT_PROFILE_SENDS executions. NOT "last seen" -- see the constant.
	uint32_t argClassIndex;
	uint64_t count;
	struct NativeCode *target;
} IcWay;

// What the emitted fast path at a site is allowed to compute WITHOUT calling
// anything, or IC_INLINE_NONE, which is what every site holds until the runtime
// proves otherwise.
//
// THE RUNTIME DECIDES THIS, NOT THE COMPILER, and that is the whole design. The
// template compiler never learns that `+` means addition -- ADR 0004 and ADR
// 0006 both turn on it not learning that, because a compiler that resolves
// arithmetic statically destroys the profile the optimizer is built to read.
// What is emitted is a sequence that asks the CELL what it may do; what fills
// the cell in is jit/Jit.c, from the primitive number of the method way 0
// actually resolved to. A program that redefines SmallInteger>>+ installs a
// method carrying no primitive, the site stops being armed, and the fast path
// stops firing -- with no name ever compared.
//
// Keyed by PRIMITIVE and not by selector for the same reason jit/Specialize.c
// is: `+` is a name, PRIM_IntAdd is what packages/Core installed.
// THE INTEGER OPERATIONS COME FIRST AND STAY CONTIGUOUS. The emitted code splits
// the two groups with one unsigned compare against IC_INLINE_INT_LAST rather
// than testing four values, because the two groups need DIFFERENT tag tests --
// tag 00 against tag 11 -- and sharing the operand load between them is what
// keeps the block from being one arm per operation.
// THE ORDER IS LOAD-BEARING, in three ways, and each one removes instructions
// from a sequence that runs at every send in the system:
//
//   * the integer operations are one contiguous run, so ONE unsigned compare
//     splits the two groups -- they need different tag tests (00 against 11) and
//     nothing else about them can be shared;
//   * the six relational operations of each group are contiguous and in the SAME
//     order in both, so the emitter builds those twelve arms from one table
//     instead of twelve hand-written blocks;
//   * within a group, everything that shares a prefix is adjacent: the three
//     that are one instruction on tagged operands, the two that share a divide,
//     the six that share a compare.
typedef enum {
	IC_INLINE_NONE = 0,

	IC_INLINE_INT_ADD,
	IC_INLINE_INT_SUB,
	IC_INLINE_INT_MUL,
	// Bitwise, and these are the cheapest of all: the SmallInteger tag is 00, so
	// it survives and, or and xor untouched and the tagged operands combine
	// directly. One instruction, no untagging, no range check -- a bitwise result
	// of two SmallIntegers is always a SmallInteger.
	IC_INLINE_INT_AND,
	IC_INLINE_INT_OR,
	IC_INLINE_INT_XOR,
	// Floor division and modulo, which share one divide.
	IC_INLINE_INT_FLOORDIV,
	IC_INLINE_INT_MOD,
	// Relational. Contiguous and ordered to match the float six below.
	IC_INLINE_INT_LT,
	IC_INLINE_INT_GT,
	IC_INLINE_INT_LE,
	IC_INLINE_INT_GE,
	IC_INLINE_INT_EQ,
	IC_INLINE_INT_NE,
	IC_INLINE_INT_LAST = IC_INLINE_INT_NE,

	IC_INLINE_FLOAT_ADD,
	IC_INLINE_FLOAT_SUB,
	IC_INLINE_FLOAT_MUL,
	IC_INLINE_FLOAT_DIV,
	IC_INLINE_FLOAT_LT,
	IC_INLINE_FLOAT_GT,
	IC_INLINE_FLOAT_LE,
	IC_INLINE_FLOAT_GE,
	IC_INLINE_FLOAT_EQ,
	IC_INLINE_FLOAT_NE,
} IcInlineOp;

// The relational six, in the order both groups list them. The emitter walks this
// to build twelve arms, and jit/Jit.c walks the same shape to arm them, so
// "which comparison" is written down once.
#define IC_INLINE_RELATIONAL_COUNT 6

typedef struct IcCell {
	RawObject *selector;
	uint64_t sends;         // total executions of this site
	// See IcInlineOp. Written by the runtime on a miss, once way 0 has settled,
	// and CLEARED by jitFlushSendCaches: a site that keeps computing the answer
	// itself never misses again, so if a method-dictionary change did not disarm
	// it here the site would serve the replaced method forever. That is the same
	// wrong ANSWER jitFlushSendCaches exists to prevent, reached by the newer
	// half of the system.
	//
	// A FULL WORD for a value that needs three bits, because the emitted code
	// reads it on every send and a 32-bit load is one instruction where a byte
	// load and a mask are two. The struct had the padding to spare.
	uint32_t inlineOp;
	// Where lookup STARTS at a `super` site: the class above the one that
	// defined the running method, as an INDEX, resolved once when the method is
	// compiled. CLASS_INDEX_INVALID at an ordinary site, and also at a super site
	// in a class with no superclass, where the send has nowhere to look and must
	// answer doesNotUnderstand rather than restart at the receiver.
	//
	// It belongs in the cell rather than being passed at the call, because the
	// call sequence already materialises the cell's address and has no spare
	// operand, and because it is a compile-time constant of the SITE.
	uint32_t lookupStart;
	uint8_t wayCount;
	// Set once more than IC_MAX_WAYS classes have been seen. PERMANENT: a site
	// that has been megamorphic has proved it has no dominant class, and
	// letting it climb back down would make the optimizer speculate on a site
	// that already refuted the speculation.
	_Bool megamorphic;
	IcWay ways[IC_MAX_WAYS];
} IcCell;

// Record one execution and answer the way it belongs to, creating it when the
// class is new and there is room.
//
// TWO IMPLEMENTATIONS OF THIS COUNTING EXIST, and that is worth stating plainly
// because it is the shape of defect this VM keeps paying for. This one runs on a
// MISS; the send site's emitted fast path (the backend's `send` operation) does
// the same three updates inline on a HIT -- the site's total, the way's count,
// and the argument's class as last seen. They have to agree, and a fast path
// that quietly stopped counting is precisely how the previous VM ended up with
// arithmetic profiles that described only the executions which had FAILED.
//
// WHAT KEEPS THEM HONEST is not review, it is a comparison anyone can run: a
// deterministic program under ST_IC_STATS=1 must report the SAME send total with
// the fast path on and with ST_NO_INLINE_CACHE=1. Change either half without the
// other and that equality breaks.
IcWay *icRecord(IcCell *cell, uint32_t receiverClass, uint32_t argumentClass);

// The way with the most executions, or NULL when the site is megamorphic or has
// never run. THE ONE WALK, which the three questions below are written in terms
// of: "which class", "which argument class" and "which method did it resolve
// to" are three readings of the same way, and three loops finding it separately
// is three places for the answer to differ.
const IcWay *icDominantWay(const IcCell *cell);

// The dominant receiver class and the fraction of executions it accounts for.
// Answers CLASS_INDEX_INVALID when the site is megamorphic or cold, which is
// exactly the case where speculating is not justified.
uint32_t icDominantClass(const IcCell *cell, double *fraction);
uint32_t icDominantArgumentClass(const IcCell *cell);
_Bool icIsMonomorphic(const IcCell *cell);

// Move the hottest way to position 0, so the inline fast path tests the class
// it will actually see. Called by the runtime on a miss; never while compiled
// code is mid-probe, because the fast path reads way 0 as a single pair.
void icPromoteHottest(IcCell *cell);

#endif
