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

	class runFloatArray: a [
		| sum dot |
		sum := 0.0.
		dot := 0.0.
		1 to: 20 do: [:pass |
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

	class runArray: a [
		| sum dot n |
		n := a size.
		sum := 0.0.
		dot := 0.0.
		1 to: 20 do: [:pass |
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
		| fa arr watch faMs arrMs |

		fa := self buildFloatArray.
		arr := self buildArray.

		watch := Stopwatch new.
		watch start.
		self check: (self runFloatArray: fa) label: 'FloatArray'.
		watch stop.
		faMs := watch elapsedMilliseconds.

		watch := Stopwatch new.
		watch start.
		self check: (self runArray: arr) label: 'Array'.
		watch stop.
		arrMs := watch elapsedMilliseconds.

		"The Array figure is the BASELINE and prints first; the FloatArray
		 figure prints LAST because scripts/ab.sh records the last
		 `<n> milliseconds` line, and the FloatArray number is the one worth
		 tracking over time."
		Transcript
			nextPutAll: 'ArrayNumeric(Array) ', arrMs printString, ' milliseconds'; lf;
			nextPutAll: 'ArrayNumeric(FloatArray) ', faMs printString, ' milliseconds'; lf
	]

]

[
	ArrayNumericBenchmark standardBenchmark.
]
