#ifndef INSTRUMENT_H
#define INSTRUMENT_H

// Execution counters for the jit-v2 work (docs/jit-v2/).
//
// The whole project's acceptance criterion is comparative: "20.000 allocations
// become 0", "zero unbox in the loop". A claim like that needs a denominator
// measured on the real VM, not an estimate, so these counters exist to produce
// the BEFORE column and, later, the AFTER column, with the SAME definitions on
// both sides. That is the only reason this header lives in vm/core/ and not in
// vm/jit/: it has to survive the engine being replaced.
//
// Compiled out by default. Build with -DST_INSTRUMENT=1 to turn it on; with it
// off every macro is (void) 0 and every JIT emission site emits nothing, so the
// cost is exactly zero instructions, not "one predictable branch".
//
// TWO RULES that the numbers depend on:
//
// 1. COUNTERS AND CLOCK NEVER COME FROM THE SAME RUN. An instrumented build
//    emits a memory RMW inside the float and integer fast paths; that is fine
//    for counting and useless for timing. Time comes from the ordinary build.
//
// 2. A COUNTER THIS BUILD DOES NOT MEASURE PRINTS "n/a", NEVER 0. A zero that
//    means "nothing incremented it" is indistinguishable from a zero that means
//    "this never happened", and the second is exactly the claim the project is
//    trying to prove. `instrumentSetUnmeasured` is how an engine declares what
//    it cannot see.
//
// Counters are non-atomic, like gIcStats next to them: exact under a single
// worker, which is what the benchmarks run. Under several workers they are a
// gauge, not a measurement.

#include <stddef.h>
#include <stdint.h>

#ifndef ST_INSTRUMENT
#define ST_INSTRUMENT 0
#endif

// Index of each counter, used for the unmeasured mask and the print order.
typedef enum {
	INSTR_ALLOCATIONS,   // objects allocated, JIT fast path + every C creator
	INSTR_ALLOC_BYTES,   // bytes those allocations took (aligned instance size)
	INSTR_HEAP_BOXES,    // doubles that did NOT fit the SmallFloat64 window
	INSTR_UNBOXES,       // tagged -> raw f64 decodes (immediate or box load)
	INSTR_TAGGED_LOADS,  // loads through a tagged pointer
	INSTR_SENDS,         // send sites executed, INCLUDING inline fast-path hits
	INSTR_IC_HITS,
	INSTR_IC_MISSES,
	INSTR_GUARD_CHECKS,
	INSTR_GUARD_FAILS,
	INSTR_DEOPTS,
	INSTR_F64_OPS,       // addsd/subsd/mulsd/divsd executed
	INSTR_I64_OPS,       // tagged integer arithmetic executed
	INSTR_SCAVENGES,
	INSTR_FULL_GCS,
	INSTR_COUNT
} InstrCounter;

typedef struct {
	uint64_t c[INSTR_COUNT];
} Instrument;

extern Instrument gInstrument;
// Bit i set = this build cannot measure counter i; it prints "n/a".
extern uint64_t gInstrumentUnmeasured;

#if ST_INSTRUMENT
#define ST_COUNT(counter)         (gInstrument.c[counter]++)
#define ST_COUNT_N(counter, n)    (gInstrument.c[counter] += (uint64_t) (n))
#define ST_INSTRUMENT_ONLY(code)  do { code } while (0)
#else
#define ST_COUNT(counter)         ((void) 0)
#define ST_COUNT_N(counter, n)    ((void) 0)
#define ST_INSTRUMENT_ONLY(code)  ((void) 0)
#endif

// Declare counters this engine does not measure, so they print "n/a" instead of
// a zero that reads like a measurement. Call once at VM init.
void instrumentSetUnmeasured(uint64_t mask);
#define ST_UNMEASURED(counter) ((uint64_t) 1 << (counter))

// Whole-run counters, printed at exit under ST_INSTRUMENT_PRINT=1.
void instrumentPrint(const char *label);
// Region counters: the benchmarks bracket their hot loop with these so setup
// and warm-up do not land in the numbers. `instrumentMark` snapshots, and
// `instrumentPrintSince` prints the delta.
void instrumentMark(void);
void instrumentPrintSince(const char *label);
uint64_t instrumentSince(InstrCounter counter);
_Bool instrumentEnabled(void);
_Bool instrumentPrintEnabled(void);
const char *instrumentName(InstrCounter counter);

#endif
