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
		"One measured run: returns elapsed milliseconds, raises on a wrong sum."

		| mark sum x i |
		mark := DateTime currentMicroTime // 1000.
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
		^(DateTime currentMicroTime // 1000) - mark
	]


	class report [
		| time count |
		count := 5.
		time := 0.
		count timesRepeat: [time := time + self run].
		Transcript
			nextPutAll: 'FloatBench hot loop ' , (time // count) printString , ' milliseconds';
			lf
	]

]


[
	FloatBench report.
]
