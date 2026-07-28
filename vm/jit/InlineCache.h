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
