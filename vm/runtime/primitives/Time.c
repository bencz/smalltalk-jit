// Time, and the heap's own numbers.

#include "runtime/primitives/Shared.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"
#include "memory/RememberedSet.h"
#include "os/Os.h"


Value primMonotonicNanos(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// A monotonic reading is 60-odd bits after a few weeks of uptime, so it can
	// legitimately outgrow the tagged window; failing is right, and the kernel's
	// own code is where a LargeInteger would come from.
	int64_t nanos = osMonotonicNanos();
	return smallIntFits(nanos) ? tagInt((intptr_t) nanos) : PRIMITIVE_FAILED;
}


Value primCurrentMicroTime(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	int64_t micros = osCurrentMicroTime();
	return smallIntFits(micros) ? tagInt((intptr_t) micros) : PRIMITIVE_FAILED;
}


Value primCollectGarbage(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// A COLLECTION FROM SMALLTALK, so the caller's compiled frames underneath
	// have to be reachable. PRIMITIVE_ALLOCATES is what anchors them, and it is
	// needed here for the same reason an allocating primitive needs it even
	// though nothing is allocated: the collector walks from the anchor.
	PRIMITIVE_ALLOCATES(args);
	collectGarbage(CurrentThread.heap);
	PRIMITIVE_DONE_ALLOCATING();
	return primitiveReceiver(args);
}


Value primPrintHeap(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	printHeap(CurrentThread.heap);
	return primitiveReceiver(args);
}


// GarbageCollector class>>lastStats: the collector's counters, as a Dictionary.
//
// STRING KEYS AND NOT SYMBOLS, because the kernel reads them with string
// literals (`stats at: 'count'`) and stringDictAtPut is the half of Dictionary
// that compares by CONTENT. Interning them would work for the write and then make
// every read miss, since a literal in a method is not the interned Symbol.
//
// WHAT IS REPORTED IS WHAT THE HEAP ACTUALLY COUNTS, which is why this is shorter
// than its v1 ancestor: that one also published fiber and waiter counts from the
// scheduler. Publishing a key whose value this VM does not track would be a number
// that reads as measured and is not, and the kernel has no way to tell the
// difference. Keys arrive when the counter behind them does.
Value primLastGCStats(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// Builds a Dictionary and a String per key, so the caller's frames are
	// anchored: any of these allocations can collect.
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Dictionary *stats = newDictionary(16);

	// A local rather than a repeated `LastGCStats.x` in each call: the entries
	// below allocate, and reading a PER_ISOLATE struct is fine across that, but
	// taking the snapshot once means every key describes the same instant instead
	// of drifting as the collector runs underneath the loop that publishes it.
	GCStats snapshot = LastGCStats;
	struct { const char *key; int64_t value; } entries[] = {
		{ "count", (int64_t) snapshot.count },
		{ "marked", (int64_t) snapshot.marked },
		{ "sweeped", (int64_t) snapshot.sweeped },
		{ "freed", (int64_t) snapshot.freed },
		{ "extended", (int64_t) snapshot.extended },
		{ "total", (int64_t) snapshot.total },
		{ "fullTimeUs", snapshot.totalTime },
		{ "scavengeCount", (int64_t) snapshot.scavengeCount },
		{ "scavengeTimeUs", snapshot.scavengeTimeUs },
		{ "youngSurvivorBytes", snapshot.youngSurvivorBytes },
		{ "oldBytes", (int64_t) CurrentThread.heap->oldSpace.allocated },
		{ "remembered",
			(int64_t) rememberedSetCount(&CurrentThread.rememberedSet) },
	};
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		// A counter that outgrew the tagged window is reported as the largest
		// value that fits rather than as a wrapped negative. None of these can
		// reach it in practice; a byte total is the one that could in principle,
		// and "saturated" is at least monotonic where a wrap is not.
		int64_t value = entries[i].value;
		if (!smallIntFits((intptr_t) value)) {
			value = (int64_t) SMALL_INT_MAX;
		}
		stringDictAtPut(stats, stringFromC(entries[i].key),
			tagInt((intptr_t) value));
	}

	Value answer = objectTagged(stats);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
