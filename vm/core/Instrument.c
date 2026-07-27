#include "core/Instrument.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Instrument gInstrument;
uint64_t gInstrumentUnmeasured;

static Instrument gMark;

static const char *gNames[INSTR_COUNT] = {
	"allocations",
	"alloc_bytes",
	"heap_boxes",
	"unboxes",
	"tagged_loads",
	"sends",
	"ic_hits",
	"ic_misses",
	"guard_checks",
	"guard_fails",
	"deopts",
	"f64_ops",
	"i64_ops",
	"scavenges",
	"full_gcs",
};


const char *instrumentName(InstrCounter counter)
{
	return gNames[counter];
}


void instrumentSetUnmeasured(uint64_t mask)
{
	gInstrumentUnmeasured |= mask;
}


_Bool instrumentEnabled(void)
{
	return ST_INSTRUMENT != 0;
}


_Bool instrumentPrintEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = ST_INSTRUMENT != 0 && getenv("ST_INSTRUMENT_PRINT") != NULL;
	}
	return enabled;
}


void instrumentMark(void)
{
	memcpy(&gMark, &gInstrument, sizeof(gMark));
}


uint64_t instrumentSince(InstrCounter counter)
{
	return gInstrument.c[counter] - gMark.c[counter];
}


// One machine-readable line per counter, so scripts/instr.sh can diff two runs
// without parsing prose. A counter this build cannot see prints `n/a`: see the
// second rule in core/Instrument.h for why that is not the same as 0.
static void printTable(const char *label, const Instrument *base)
{
	// An uninstrumented build must say so, not print a table of zeros. A zero
	// here would read exactly like the measurement the project is trying to
	// prove ("zero allocations in the loop") while meaning "nothing counts".
	if (!ST_INSTRUMENT) {
		printf("#instr unavailable  build without -DST_INSTRUMENT=1\n");
		fflush(stdout);
		return;
	}
	printf("#instr %s\n", label == NULL ? "run" : label);
	for (int i = 0; i < INSTR_COUNT; i++) {
		if ((gInstrumentUnmeasured >> i) & 1) {
			printf("#instr %-14s n/a\n", gNames[i]);
		} else {
			printf("#instr %-14s %llu\n", gNames[i],
				(unsigned long long) (gInstrument.c[i] - (base == NULL ? 0 : base->c[i])));
		}
	}
	fflush(stdout);
}


void instrumentPrint(const char *label)
{
	printTable(label, NULL);
}


void instrumentPrintSince(const char *label)
{
	printTable(label, &gMark);
}
