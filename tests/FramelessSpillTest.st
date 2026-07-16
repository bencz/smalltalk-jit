"Regression: a FRAMELESS method that spills.

 A method with no send, no block, no context var and no outer return is compiled
 frameless: generateCode skips generatePrologue entirely, so RBP is never established
 and NO spill area is reserved (or nil-initialised) -- generatePrologue is what does
 both. But scanCode decides frameless from the bytecodes alone, while scanRegisters
 runs afterwards and spills whenever a method outgrows the register pool. The two
 decisions are not independent: a spilled variable needs a frame.

 When they disagreed, every spill accessor except fillVar addressed the slot off the
 frame pointer, which in a frameless method still belongs to the CALLER. So the spill
 wrote through the caller's frame: it lost the store (this test's symptom -- an
 instance variable left nil) and clobbered the caller's saved frame pointer and return
 address, which returned into the middle of an instruction. The fix is in the shared
 computeRegsAlloc: if anything spilled, the method is not frameless.

 Reachable at the DEFAULT pool, not just under ST_JIT_REGS: setTen: below is a pure
 setter with 10 args + self = 11 variables against a 9-register pool. Found via
 samples/concurrency/07_mediator_cqrs.st, whose 8-argument OrderResult setter sits
 right at the edge.

 Run under the pressure sweep too, where far smaller methods take this path:

     for n in 3 4 5 6 7 8 9; do ST_JIT_REGS=$n ./run_tests.sh --all; done"

FramelessSpill := Object [
	| a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 |

	"No sends here, so this is frameless, and 10 args + self outgrow the pool."
	setTen: x1 b: x2 c: x3 d: x4 e: x5 f: x6 g: x7 h: x8 i: x9 j: x10 [
		a1 := x1. a2 := x2. a3 := x3. a4 := x4. a5 := x5.
		a6 := x6. a7 := x7. a8 := x8. a9 := x9. a10 := x10.
	]

	sum [ ^a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 ]

	"Frameless with temps rather than args: reads back every one, so a lost store or a
	 slot read off the wrong base shows up as a wrong sum."
	tempChurn [
		| t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 |
		t1 := 1. t2 := 2. t3 := 3. t4 := 4. t5 := 5. t6 := 6.
		t7 := 7. t8 := 8. t9 := 9. t10 := 10. t11 := 11. t12 := 12.
		^t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + t10 + t11 + t12
	]
]

"The caller matters as much as the callee: if the frameless callee spills through the
 caller's frame, the CALLER's own variables are what get corrupted."
FramelessCaller := Object [
	drive [
		| target keep1 keep2 keep3 |
		keep1 := 100. keep2 := 200. keep3 := 300.
		target := FramelessSpill new.
		target setTen: 1 b: 2 c: 3 d: 4 e: 5 f: 6 g: 7 h: 8 i: 9 j: 10.
		"keep1..3 must survive the call intact"
		^target sum + keep1 + keep2 + keep3
	]
]

[
	| fails check |
	fails := 0.

	check := [:name :expected :block |
		[block value = expected
			ifFalse: [
				fails := fails + 1.
				Transcript nextPutAll: 'FAIL (wrong value): '; nextPutAll: name; lf]]
			on: Error
			do: [:e |
				fails := fails + 1.
				Transcript nextPutAll: 'FAIL (raised): '; nextPutAll: name; lf]].

	check value: 'setTen/sum' value: 55 value: [
		| r | r := FramelessSpill new.
		r setTen: 1 b: 2 c: 3 d: 4 e: 5 f: 6 g: 7 h: 8 i: 9 j: 10.
		r sum].
	check value: 'tempChurn' value: 78 value: [FramelessSpill new tempChurn].
	check value: 'callerSurvives' value: 655 value: [FramelessCaller new drive].

	fails = 0 ifTrue: [Transcript nextPutAll: 'FramelessSpillTest: OK'; lf].
	^fails
]
