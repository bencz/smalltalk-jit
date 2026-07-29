#include "jit/InlineCache.h"
#include "core/ClassTable.h"
#include "core/Assert.h"


IcWay *icRecord(IcCell *cell, uint32_t receiverClass, uint32_t argumentClass)
{
	cell->sends++;
	for (uint8_t i = 0; i < cell->wayCount; i++) {
		if (cell->ways[i].classIndex == receiverClass) {
			cell->ways[i].count++;
			// The argument class is kept as the LAST seen for this receiver
			// class rather than as a full histogram. One word instead of a
			// table, and it is enough for the decision it feeds: a site whose
			// argument class actually varies is one where speculating on it
			// would be wrong anyway, and the guard catches that.
			cell->ways[i].argClassIndex = argumentClass;
			return &cell->ways[i];
		}
	}
	if (cell->wayCount == IC_MAX_WAYS) {
		// Permanent. A site that has seen seven classes has demonstrated it has
		// no dominant one, and letting it climb back down would make the
		// optimizer speculate on a site that already refuted the speculation.
		cell->megamorphic = 1;
		return NULL;
	}
	IcWay *way = &cell->ways[cell->wayCount++];
	way->classIndex = receiverClass;
	way->argClassIndex = argumentClass;
	way->count = 1;
	way->target = NULL;
	return way;
}


uint32_t icDominantClass(const IcCell *cell, double *fraction)
{
	if (fraction != NULL) {
		*fraction = 0.0;
	}
	if (cell->megamorphic || cell->wayCount == 0 || cell->sends == 0) {
		return CLASS_INDEX_INVALID;
	}
	const IcWay *best = &cell->ways[0];
	for (uint8_t i = 1; i < cell->wayCount; i++) {
		if (cell->ways[i].count > best->count) {
			best = &cell->ways[i];
		}
	}
	if (fraction != NULL) {
		*fraction = (double) best->count / (double) cell->sends;
	}
	return best->classIndex;
}


uint32_t icDominantArgumentClass(const IcCell *cell)
{
	if (cell->megamorphic || cell->wayCount == 0) {
		return CLASS_INDEX_INVALID;
	}
	const IcWay *best = &cell->ways[0];
	for (uint8_t i = 1; i < cell->wayCount; i++) {
		if (cell->ways[i].count > best->count) {
			best = &cell->ways[i];
		}
	}
	return best->argClassIndex;
}


_Bool icIsMonomorphic(const IcCell *cell)
{
	return !cell->megamorphic && cell->wayCount == 1;
}


void icPromoteHottest(IcCell *cell)
{
	if (cell->wayCount < 2) {
		return;
	}
	// The emitted cache tests ways 0..IC_EMITTED_WAYS-1 in order, so those
	// positions have to HOLD the hottest classes or the fast path spends its
	// compares on classes the site rarely sees.
	//
	// A selection pass over just those positions, not a sort of the whole array:
	// what is below them is never read by compiled code, so ordering it would be
	// work with no reader.
	uint8_t ordered = IC_EMITTED_WAYS < cell->wayCount
		? IC_EMITTED_WAYS : cell->wayCount;
	for (uint8_t position = 0; position < ordered; position++) {
		uint8_t hottest = position;
		for (uint8_t i = (uint8_t) (position + 1); i < cell->wayCount; i++) {
			if (cell->ways[i].count > cell->ways[hottest].count) {
				hottest = i;
			}
		}
		if (hottest != position) {
			IcWay swap = cell->ways[position];
			cell->ways[position] = cell->ways[hottest];
			cell->ways[hottest] = swap;
		}
	}
}
