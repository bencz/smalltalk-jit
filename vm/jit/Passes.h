#ifndef PASSES_H
#define PASSES_H

// The optimizer. See Passes.c for why the ORDER matters more than the passes.

#include "jit/Ir.h"

typedef struct {
	uint32_t trivialPhis;
	uint32_t typesLearned;
	uint32_t guardsRemoved;
	uint32_t simplified;    // box/unbox pairs and field-of-new reads
	uint32_t gvnRemoved;
	uint32_t scalarReplaced;
	uint32_t materializations;
	uint32_t hoisted;
	uint32_t phisPromoted;
	uint32_t deadRemoved;
	uint32_t blocksMerged;
	uint32_t allocationsRemaining;
} PassStats;

PassStats irOptimize(IrFunction *function);

#endif
