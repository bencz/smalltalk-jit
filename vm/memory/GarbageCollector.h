#ifndef GARBAGECOLLECTOR_H
#define GARBAGECOLLECTOR_H

#include "core/Object.h"
#include "core/Thread.h"
#include "memory/HeapPage.h"

typedef struct {
	size_t count;
	size_t total;
	size_t marked;
	size_t sweeped;
	size_t freed;
	size_t extended;
	int64_t time;
	int64_t totalTime;
	// Scavenge (minor GC) counters. Cumulative — NOT cleared by resetGcStats, so
	// deltas across stress waves reveal whether per-scavenge cost is climbing
	// (the never-pruned-remembered-set symptom).
	size_t scavengeCount;
	int64_t scavengeTimeUs;
	int64_t youngSurvivorBytes;   // bytes copied within young by the last scavenge
} GCStats;

extern PER_ISOLATE GCStats LastGCStats;

void gcMarkRoots(Thread *thread);
void gcSweep(PageSpace *space);
void resetGcStats(void);
void printGcStats(void);

#endif
