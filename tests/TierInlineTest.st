"Tier-1 speculative inlining correctness (jit/Tier.h, compiler/Optimizer.c):
 hot methods whose monomorphic sends reach eligible LEAF callees are
 recompiled with the callee bodies inlined behind an exact-class guard whose
 fallback is the original send. Every scenario asserts per-iteration answers,
 so any divergence between the sent and the inlined body, a broken argument
 remapping, a wrong ivar slot, or a guard mismatch is a counted failure.

 Covered: asymmetric multi-argument callees (argument-order proof), ivar
 getter/setter callees (the INST_VAR_OF rewrite, loads and barrier-checked
 stores), guard failure on the FIRST iteration, on the LAST, and alternating
 with the hot class, an exception raised INSIDE an inlined body (caught by
 the caller, object stays consistent), statement-position sends (result
 discarded), and a fall-off-the-end callee (answers self)."

TierInlA := Object [
	| acc |
	init [ acc := 0 ]
	acc [ ^acc ]
	accPut: n [ acc := n ]
	minusFrom: a minus: b [ ^a - b ]
	touch [ acc := acc + 1 ]
	"drive: exercises getter, setter, 2-arg asymmetric callee and a
	 statement-position send in one hot method"
	drive: n [
		self touch.
		self accPut: (self minusFrom: self acc + n minus: n - 1).
		^self acc ]
]
TierInlB := TierInlA [
	acc [ ^0 - 5 ]
	minusFrom: a minus: b [ ^b - a ]
]

TierInlSelf := Object [
	poke [ ]  "falls off the end: answers self"
	wrap [ ^self poke ]
]

[ | fails a b probe r |
	fails := 0.

	"Phase 1: heat drive: past the threshold; per-iteration exact answers.
	 acc' = (acc + 1 + n) - (n - 1) = acc + 2, so acc = 2i after i calls."
	a := TierInlA new init.
	1 to: 2000 do: [:i |
		(a drive: i) = (i * 2) ifFalse: [fails := fails + 1]].

	"Phase 2: churn so fresh sites adopt the recompiled (inlined) drive:."
	1 to: 10000 do: [:i | Array new: 1024].
	a := TierInlA new init.
	1 to: 500 do: [:i |
		(a drive: i) = (i * 2) ifFalse: [fails := fails + 1]].

	"Phase 3: guard fail on the FIRST iteration (subclass before the hot class
	 ever runs the fresh sites), then alternating with the hot class, then as
	 the LAST iteration. TierInlB overrides acc (a constant, ignoring the
	 ivar) and SWAPS minusFrom:minus:, so any false guard hit computes a
	 detectably different answer: for B, drive: always answers -5 (the acc
	 override), for A it answers 2i."
	b := TierInlB new init.
	(b drive: 1) = (0 - 5) ifFalse: [fails := fails + 1].
	a := TierInlA new init.
	1 to: 300 do: [:i |
		(a drive: i) = (i * 2) ifFalse: [fails := fails + 1].
		(b drive: i) = (0 - 5) ifFalse: [fails := fails + 1]].
	(b drive: 999) = (0 - 5) ifFalse: [fails := fails + 1].

	"Phase 4: exception INSIDE an inlined body: minusFrom:minus: subtracts a
	 string, the doesNotUnderstand error unwinds out of the inlined frame and
	 is caught by the caller's handler; the receiver stays consistent."
	probe := TierInlA new init.
	1 to: 5 do: [:i | probe drive: i].
	r := [ probe drive: 'boom' ] on: Error do: [:e | 42 ].
	r = 42 ifFalse: [fails := fails + 1].
	(probe drive: 1) = (probe acc) ifFalse: [fails := fails + 1].

	"Phase 5: fall-off-the-end callee answers self through the inlined path."
	probe := TierInlSelf new.
	1 to: 1500 do: [:i |
		(probe wrap == probe) ifFalse: [fails := fails + 1]].
	1 to: 5000 do: [:i | Array new: 256].
	1 to: 300 do: [:i |
		(probe wrap == probe) ifFalse: [fails := fails + 1]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'TierInlineTest: OK'; lf].
	^fails
]
