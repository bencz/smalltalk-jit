#ifndef SPECIALIZE_H
#define SPECIALIZE_H

// Turning a method's TYPE PROFILE into the optimizer's specialization table.
//
// The one place that knows "IntAddPrimitive means IR_IADD", and it is one place
// on purpose: what a primitive number means is a fact about packages/Core, and
// this VM has paid repeatedly for facts about packages/ being spelled out in two
// halves that then drift apart.
//
// WHAT IT REFUSES TO ASSUME, and this is the part that separates a correct
// specialization from a plausible one: it never decides from the SELECTOR. A
// site sending `+` to a SmallInteger is specialized only when the method that
// site actually RESOLVED TO is the one carrying IntAddPrimitive. Deciding by
// name would compile `+` into an addition for a program that redefined
// SmallInteger>>+, and the answer would be wrong with nothing to notice it --
// the guard would still hold, because the receiver really is a SmallInteger.

#include "jit/Passes.h"

struct NativeCode;

// Build the table for `code`, whose cells carry the profile. Answers a
// malloc'd array of `code->unit->instructionCount` entries, every one of them
// "do not specialize" unless the site proved otherwise, or NULL when the method
// has no sites worth the allocation. The caller frees it.
//
// `specialized` receives how many sites were decided, which is the measurement
// the whole item exists to produce: a table with zero entries is the honest
// report that this method's arithmetic is not monomorphic.
SiteSpecialization *specializeFor(struct NativeCode *code,
	uint32_t *specialized);

// The share of a site's executions its dominant class must account for before
// speculating on it. See the definition for why it is not 100%.
double specializeDominanceThreshold(void);

#endif
