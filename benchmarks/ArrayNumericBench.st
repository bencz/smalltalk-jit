"FloatArray (unboxed doubles in a byte payload, bulk operations in C) against
 the same numeric work over an Array of Float objects.

 The Array path is what this VM offered before FloatArray existed, and it pays,
 per element: a message send for at:, a tag decode, and for any value outside
 the immediate SmallFloat64 range a heap allocation. It also hands the collector
 one scanned word per element on every cycle. The FloatArray path pays none of
 that; sum and dot: are single C loops over a raw buffer.

 Both sides compute the SAME quantities and the results are cross-checked
 against each other AND against closed forms, so a printed figure means the two
 representations agreed:

   elements 1..N with value i/8.0 (a Float on BOTH sides, so the two
   representations do the same arithmetic and only the storage differs)
   sum      = (1/8) * N(N+1)/2
   dot      = sum of (i/8)^2 = (1/64) * N(N+1)(2N+1)/6

 Both are exactly representable in a double for N = 100000 (the largest
 intermediate is about 5.2e10, well inside 2^53), so the comparison is exact and
 not a tolerance check.

 Original to this repository; carries the repository's own licence, unlike
 Richards and DeltaBlue."

ArrayNumericBenchmark := Object [

	class size [ ^100000 ]

	class expectedSum [
		| n |
		n := self size.
		^n * (n + 1) / 2 / 8
	]

	class expectedDot [
		| n |
		n := self size.
		^n * (n + 1) * (2 * n + 1) / 6 / 64
	]

	"--- FloatArray ---"

	class buildFloatArray [
		| a n |
		n := self size.
		a := FloatArray new: n.
		1 to: n do: [:i | a at: i put: i / 8.0].
		^a
	]

	class runFloatArray: a passes: passes [
		| sum dot |
		sum := 0.0.
		dot := 0.0.
		1 to: passes do: [:pass |
			sum := a sum.
			dot := a dot: a].
		^Array with: sum with: dot
	]

	"--- Array of Float objects, the pre-FloatArray way ---"

	class buildArray [
		| a n |
		n := self size.
		a := Array new: n.
		1 to: n do: [:i | a at: i put: i / 8.0].
		^a
	]

	class runArray: a passes: passes [
		| sum dot n |
		n := a size.
		sum := 0.0.
		dot := 0.0.
		1 to: passes do: [:pass |
			sum := 0.0.
			dot := 0.0.
			1 to: n do: [:i | | v |
				v := a at: i.
				sum := sum + v.
				dot := dot + (v * v)]].
		^Array with: sum with: dot
	]

	class check: result label: label [
		| sum dot |
		sum := result at: 1.
		dot := result at: 2.
		sum = self expectedSum ifFalse: [
			^self error: label, ' sum WRONG: got ', sum printString,
				' expected ', self expectedSum printString].
		dot = self expectedDot ifFalse: [
			^self error: label, ' dot WRONG: got ', dot printString,
				' expected ', self expectedDot printString].
		^true
	]

	class standardBenchmark [
		"The two sides get DIFFERENT pass counts on purpose. They differ by more
		 than an order of magnitude, so one count cannot put both in a
		 measurable range without making the slow side dominate the whole gate:
		 at the old 20 passes the FloatArray side took 3 ms and every row ever
		 recorded in benchmarks/results/BASELINE.jsonl reads exactly 3, which is
		 no signal at all. Each side is scaled to land near 200 ms on its own,
		 and the per-pass figures below are what make them comparable."

		| fa arr watch faMs arrMs faPasses arrPasses |
		faPasses := 1000.
		arrPasses := 100.

		fa := self buildFloatArray.
		arr := self buildArray.

		watch := Stopwatch new.
		watch start.
		self check: (self runFloatArray: fa passes: faPasses) label: 'FloatArray'.
		watch stop.
		faMs := watch elapsedMilliseconds.

		watch := Stopwatch new.
		watch start.
		self check: (self runArray: arr passes: arrPasses) label: 'Array'.
		watch stop.
		arrMs := watch elapsedMilliseconds.

		"The Array figure is the BASELINE and prints first; the FloatArray
		 figure prints LAST because scripts/ab.sh records the last
		 `<n> milliseconds` line, and the FloatArray number is the one worth
		 tracking over time."
		Transcript
			nextPutAll: 'ArrayNumeric(Array) ', arrMs printString, ' milliseconds (',
				arrPasses printString, ' passes)'; lf;
			nextPutAll: 'ArrayNumeric(FloatArray) ', faMs printString, ' milliseconds (',
				faPasses printString, ' passes)'; lf
	]

]

[
	ArrayNumericBenchmark standardBenchmark.
]
