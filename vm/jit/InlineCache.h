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

typedef struct {
	uint32_t classIndex;    // receiver class this way matches
	uint32_t argClassIndex; // dominant class of the first argument here
	uint64_t count;
	struct NativeCode *target;
} IcWay;

typedef struct IcCell {
	RawObject *selector;
	uint64_t sends;         // total executions of this site
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
