"A monotonic elapsed-time stopwatch built on the platform monotonic clock
 (nanoseconds, immune to wall-clock adjustments). start/stop accumulate; reset
 clears; elapsed answers a Duration. The class-side millisecondsToRun: and
 timeToRun: time a single block."

Stopwatch := Object [

	| startNanos accumulated running |


	"--- instance creation ---"

	class new [
		^super new initStopwatch
	]


	class start [
		^self new start
	]


	class monotonicNanos [
		<primitive: MonotonicNanoTimePrimitive>
	]


	initStopwatch [
		accumulated := 0.
		running := false.
		^self
	]


	"--- controlling ---"

	start [
		running ifFalse: [
			startNanos := Stopwatch monotonicNanos.
			running := true].
		^self
	]


	stop [
		running ifTrue: [
			accumulated := accumulated + (Stopwatch monotonicNanos - startNanos).
			running := false].
		^self
	]


	reset [
		accumulated := 0.
		running := false.
		^self
	]


	restart [
		accumulated := 0.
		startNanos := Stopwatch monotonicNanos.
		running := true.
		^self
	]


	"--- reading ---"

	isRunning [
		^running
	]


	elapsedNanos [
		^running
			ifTrue: [accumulated + (Stopwatch monotonicNanos - startNanos)]
			ifFalse: [accumulated]
	]


	elapsedMilliseconds [
		^self elapsedNanos // 1000000
	]


	elapsed [
		^Duration milliseconds: self elapsedMilliseconds
	]


	"--- timing a block (class side) ---"

	class millisecondsToRun: aBlock [
		| t0 |

		t0 := self monotonicNanos.
		aBlock value.
		^(self monotonicNanos - t0) // 1000000
	]


	class timeToRun: aBlock [
		^Duration milliseconds: (self millisecondsToRun: aBlock)
	]

]
