"MIXED-TYPE arithmetic: the same hot loop as FloatBench, but with the integer
 literals left as integers so every +, - and > crosses the type boundary.

 This is the benchmark for the coercion path. Same-type arithmetic has a JIT
 call-site intrinsic and costs a few nanoseconds; a mixed operand pair misses
 that intrinsic, misses the C primitive (which rejects a non-float argument),
 and lands in Number>>retry:coercing:. Measured on the tree this file was
 written against, one million iterations cost 5 ms for float+float and 523 ms
 for float+int, a factor of about 100.

 expectedSum below was produced by a C program performing the IDENTICAL
 operation sequence on IEEE-754 doubles, so the comparison is bit-exact:
 mixed arithmetic is specified to coerce the integer to a double and then do an
 ordinary double operation, and any deviation is a miscompile, not a rounding
 difference. Same convention as FloatBench.st.

   double x = 0.5, sum = 0.0;
   for (long i = 1; i <= 2000000; i++) {
       x = x + 1;
       sum = sum + x / (x + 1);
       if (x > 1000) { x = x - 1000; }
   }
   -> sum = 1987586.8066327267, x = 0.5

 x walks 1.5, 2.5, ... 1000.5 and then wraps, so every value is an exact
 half and stays inside the immediate SmallFloat64 range: this measures the
 COERCION, never boxing.

 Unlike Richards and DeltaBlue this file is original to this repository and
 carries the repository's own licence.

 WARNING: report:times:run: divides the total by the iteration count using
 integer division, so a benchmark that runs in under a millisecond per
 iteration reports 0. This one deliberately reports the TOTAL of a single
 timed run instead, which is what gives it usable resolution for A/B work."

MixedArithBenchmark := Object [

	class run [
		| x sum i n |
		n := 2000000.
		x := 0.5.
		sum := 0.0.
		i := 1.
		[i <= n] whileTrue: [
			x := x + 1.
			sum := sum + (x / (x + 1)).
			x > 1000 ifTrue: [x := x - 1000].
			i := i + 1].
		^Array with: sum with: x
	]

	class standardBenchmark [
		"REPEATS the run rather than raising the iteration count inside it: the
		 2000000 in `run` is baked into expectedSum by the C reference program
		 quoted at the top of this file, so changing it would destroy the
		 bit-exact check. Ten repeats put the total around 310 ms, which is long
		 enough for the 1 ms printing resolution to be 0.3%; each repeat is
		 verified on its own."

		| watch result sum x expectedSum repeats |
		expectedSum := 1987586.8066327267.
		repeats := 10.

		watch := Stopwatch new.
		watch start.
		repeats timesRepeat: [
			result := self run.

			sum := result at: 1.
			x := result at: 2.
			sum = expectedSum ifFalse: [
				^self error: 'MixedArithBench WRONG: got ', sum printString,
					' expected ', expectedSum printString].
			x = 0.5 ifFalse: [
				^self error: 'MixedArithBench WRONG: x is ', x printString, ' expected 0.5']].
		watch stop.

		Transcript
			nextPutAll: 'MixedArith ', watch elapsedMilliseconds printString, ' milliseconds';
			lf
	]

]

[
	MixedArithBenchmark standardBenchmark.
]
