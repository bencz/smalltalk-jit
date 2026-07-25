"Float arithmetic hot loop: add, div, compare and sub on values that fit the
immediate SmallFloat64 range, so it measures exactly the path the immediate
float work optimizes. The expected sum was computed by a C program running the
IDENTICAL operation sequence on IEEE-754 doubles, so the comparison is
bit-exact, any deviation is a miscompile. Unlike Richards/DeltaBlue this file
is original to this repository and carries the repository license."

FloatBench := Object [

	class expectedSum [
		^1986820.5889376509
	]


	class run [
		"One measured run: returns elapsed MICROseconds, raises on a wrong sum.

		 Microseconds, not milliseconds: a run is about 28 ms, so a millisecond
		 sample truncated away 3.5% and the caller then divided by the run count
		 on top of that. See report below."

		| mark sum x i |
		mark := DateTime currentMicroTime.
		sum := 0.0.
		x := 0.0.
		i := 0.
		[i < 2000000] whileTrue: [
			x := x + 0.25.
			sum := sum + (x / (x + 1.0)).
			x > 1000.0 ifTrue: [x := x - 1000.0].
			i := i + 1].
		sum = self expectedSum ifFalse: [
			self error: 'FloatBench sum mismatch: ' , sum printString].
		^DateTime currentMicroTime - mark
	]


	class report [
		"TOTAL of the batch, not the mean: see the note in Richards.st's
		 report:times:run:. 12 runs rather than 5 so the total lands around
		 340 ms and a 1 ms printing tick is 0.3%."

		| micros count |
		count := 12.
		micros := 0.
		count timesRepeat: [micros := micros + self run].
		Transcript
			nextPutAll: 'FloatBench hot loop ' , (micros // 1000) printString , ' milliseconds';
			lf
	]

]


[
	FloatBench report.
]
