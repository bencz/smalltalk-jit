"Dot product over a generic Array of pointers to Vec3.

 This is the jit-v2 acceptance benchmark in the only form the CURRENT VM can
 express it: `Vec3` is an ordinary object with tagged instance variables, and
 the array holds one pointer per element. The flat-layout twin (`vec3_flat`, a
 Value class whose fields are raw doubles embedded in an array with no header
 and no pointer per element) does not exist yet, because value classes are
 phase 7. The difference between the two is what the project is buying, so THIS
 file is the denominator.

 Element values, iteration order and accumulation order are identical to the
 reference prototype (python_proto/vec3.st), so the numeric result must be
 IDENTICAL: 149876875.0 for 200 elements and 50 iterations. That is a real
 cross-check between two independent implementations, not a self-consistency
 test, and it is also the `resultado numerico identico ao do JIT atual` clause
 of the acceptance criterion.

 Written for this repository; carries the repository license, unlike
 Richards.st and DeltaBlue.st."

Instr := Object [
	"Region bracketing for the execution counters (vm/core/Instrument.h). Both
	 primitives are no-ops unless the VM was built with -DST_INSTRUMENT=1, so
	 this file runs unchanged on an ordinary build."

	class mark [
		<primitive: InstrumentMarkPrimitive>
		^self
	]

	class report [
		<primitive: InstrumentReportPrimitive>
		^self
	]
]

Vec3 := Object [
	| x y z |

	class x: ax y: ay z: az [
		^self new setX: ax y: ay z: az
	]

	setX: ax y: ay z: az [
		x := ax. y := ay. z := az
	]

	x [ ^x ]
	y [ ^y ]
	z [ ^z ]

	dot: o [
		^(x * o x) + (y * o y) + (z * o z)
	]

	scale: s [
		^Vec3 x: x * s y: y * s z: z * s
	]
]

Vec3Boxed := Object [
	| items n |

	class expectedTotal [
		"Closed form, so the check is independent of the loop that computes it.
		 Element i (0-based) is (i, i+1, i/2), and the body accumulates
		 v dot: (v scale: 0.5) = 0.5 * (x*x + y*y + z*z) = 1.125i^2 + i + 0.5.
		 Summed over i in [0, 200) and multiplied by 50 iterations."
		^149876875.0
	]

	setup: count [
		n := count.
		items := Array new: count.
		1 to: count do: [ :j |
			| i |
			i := j - 1.
			items at: j put: (Vec3 x: i asFloat
			                      y: i asFloat + 1.0
			                      z: i asFloat * 0.5) ].
		^count
	]

	hot: iters [
		| total v w |
		total := 0.0.
		1 to: iters do: [ :k |
			1 to: n do: [ :i |
				v := items at: i.
				w := v scale: 0.5.
				total := total + (v dot: w) ] ].
		^total
	]

	class report [
		"Counters and clock measure DIFFERENT regions, on purpose.

		 The counters cover exactly ONE canonical run: 200 elements, 50
		 iterations, which is the size the acceptance criterion names, so the
		 numbers are directly comparable with the reference prototype's table.
		 Setup and warm-up stay outside the mark, because the 200 Vec3 the setup
		 builds would otherwise be indistinguishable from allocations the hot
		 loop failed to eliminate.

		 The clock covers a BATCH, because one canonical run is about 0.5 ms and
		 a millisecond tick would quantize it to nothing. See the note in
		 benchmarks/README.md about the integer-division mean that cost this
		 project its wall-clock signal for its whole history."
		| bench total micros batch |
		batch := 500.
		bench := self new.
		bench setup: 200.
		"Warm-up must OUTLAST the tier-1 trigger, or the recompilation fires
		 inside the marked region and the counters wobble by a few allocations
		 from run to run (measured: 10100 vs 10094). The default threshold is
		 1000 invocations (ST_TIER_THRESHOLD), and `hot: n` calls the inner
		 methods 200n times, so 10 iterations puts every hot method past it
		 before the mark. What we want counted is the STEADY STATE."
		bench hot: 10.

		Instr mark.
		total := bench hot: 50.
		Instr report.
		total = self expectedTotal ifFalse: [
			self error: 'Vec3Boxed total mismatch: ' , total printString ].

		micros := DateTime currentMicroTime.
		batch timesRepeat: [
			total := bench hot: 50.
			total = self expectedTotal ifFalse: [
				self error: 'Vec3Boxed total mismatch: ' , total printString ] ].
		micros := DateTime currentMicroTime - micros.

		Transcript
			nextPutAll: 'Vec3Boxed ' , (micros // 1000) printString , ' milliseconds';
			lf
	]
]


[
	Vec3Boxed report.
]
