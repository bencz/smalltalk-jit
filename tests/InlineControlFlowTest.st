"Tier-1 speculative inlining of callees that contain CONTROL FLOW.

 The inliner used to reject any callee containing a jump, and the compiler
 turns every ifTrue:, ifFalse:, and:, or:, whileTrue: and to:do: into jumps, so
 in practice only accessors and straight-line arithmetic were ever inlined.
 Lifting that means the callee's own jumps must be relocated into the caller's
 instruction stream, which is where this file aims:

  * a callee jump target is a byte offset in the CALLEE, and the caller uses
    the same units. A callee offset resolved against the caller's map yields
    some unrelated caller instruction, which is a silently wrong jump rather
    than a crash, so every shape below asserts a per-iteration VALUE.
  * a RETURN is no longer restricted to the tail, so several returns can leave
    one inlined body. Each needs its own exit (an AssemblerLabel takes exactly
    one forward reference) and each must skip the `answers self` copy that
    falling off the end emits, or the returned value is clobbered.
  * a callee loop puts a BACK EDGE inside the caller, which the register
    allocator's liveness and the GC stackmap widening must survive. The loop
    cases here allocate inside the loop so a scavenge can land there.

 Every case runs past the tier threshold and asserts the answer on EVERY
 iteration, before and after recompilation, so a divergence between the sent
 and the inlined form is a counted failure rather than a lucky sample.

 WARNING for whoever extends this: a callee that branches on one of its own
 INSTANCE VARIABLES is deliberately still rejected. adjustOperand rewrites an
 ivar into OPERAND_INST_VAR_OF, and generateClassCheck has no arm for that form
 on either backend, so it would hit the default FAIL(). InlineIvarBranch below
 pins that this stays correct (it must answer right whether or not it inlines)."

"--- conditionals, both arms, plus a non-tail return ---"
InlineCfA := Object [
	classify: n [
		n > 10 ifTrue: [ ^#big ].
		n > 0 ifTrue: [ ^#small ].
		^#negative
	]
	drive: n [ ^self classify: n ]
]

"--- and:/or:, which compile to jumps too ---"
InlineCfB := Object [
	inRange: n [ ^(n > 0) and: [ n < 100 ] ]
	either: n [ ^(n < 0) or: [ n > 100 ] ]
	driveIn: n [ ^self inRange: n ]
	driveEither: n [ ^self either: n ]
]

"--- a LOOP in the callee: a back edge inside the caller ---"
InlineCfC := Object [
	sumTo: n [
		| total i |
		total := 0.
		i := 1.
		[ i <= n ] whileTrue: [ total := total + i. i := i + 1 ].
		^total
	]
	drive: n [ ^self sumTo: n ]
]

"--- a loop that ALLOCATES, so a scavenge can happen on the back edge ---"
InlineCfD := Object [
	build: n [
		| acc i |
		acc := OrderedCollection new.
		i := 1.
		[ i <= n ] whileTrue: [ acc add: (Array with: i with: i). i := i + 1 ].
		^acc size
	]
	drive: n [ ^self build: n ]
]

"--- to:do:, and a callee that falls off the end (answers self) ---"
InlineCfE := Object [
	| count |
	init [ count := 0. ^self ]
	count [ ^count ]
	bump: n [ 1 to: n do: [:i | count := count + i ] ]
	drive: n [ ^self bump: n ]
]

"--- several returns, none of them in tail position ---"
InlineCfF := Object [
	pick: n [
		n = 1 ifTrue: [ ^#one ].
		n = 2 ifTrue: [ ^#two ].
		n = 3 ifTrue: [ ^#three ].
		^#other
	]
	drive: n [ ^self pick: n ]
]

"--- guard failure: the SAME inlined body reached by another class ---"
InlineCfSub := InlineCfA [
	classify: n [ ^#overridden ]
]

"--- an exception raised from INSIDE an inlined body with control flow ---"
InlineCfG := Object [
	risky: n [
		n > 5 ifTrue: [ ^Error signal: 'too big' ].
		^n * 2
	]
	drive: n [ ^self risky: n ]
]

"--- branches on an INSTANCE VARIABLE: must stay correct, inlined or not ---"
InlineIvarBranch := Object [
	| flag |
	setFlag: aBoolean [ flag := aBoolean. ^self ]
	choose [ flag ifTrue: [ ^#yes ]. ^#no ]
	drive [ ^self choose ]
]

[ | t a b c d e f g sub bad hot |
	t := TestRun named: 'InlineControlFlow'.
	hot := 3000.

	a := InlineCfA new.
	b := InlineCfB new.
	c := InlineCfC new.
	d := InlineCfD new.
	f := InlineCfF new.
	g := InlineCfG new.
	sub := InlineCfSub new.

	"--- conditionals with returns from several arms ---"
	bad := 0.
	1 to: hot do: [:i |
		(a drive: 50) == #big ifFalse: [bad := bad + 1].
		(a drive: 5) == #small ifFalse: [bad := bad + 1].
		(a drive: -5) == #negative ifFalse: [bad := bad + 1]].
	t check: 'every arm of an inlined conditional answers correctly, hot'
		equals: 0 is: [ bad ].

	"--- and: / or: ---"
	bad := 0.
	1 to: hot do: [:i |
		(b driveIn: 50) ifFalse: [bad := bad + 1].
		(b driveIn: 500) ifTrue: [bad := bad + 1].
		(b driveIn: -1) ifTrue: [bad := bad + 1].
		(b driveEither: -1) ifFalse: [bad := bad + 1].
		(b driveEither: 50) ifTrue: [bad := bad + 1]].
	t check: 'and:/or: inline correctly, hot' equals: 0 is: [ bad ].

	"--- a loop inside the callee: a back edge in the caller ---"
	bad := 0.
	1 to: hot do: [:i |
		(c drive: 10) = 55 ifFalse: [bad := bad + 1].
		(c drive: 0) = 0 ifFalse: [bad := bad + 1].
		(c drive: 1) = 1 ifFalse: [bad := bad + 1]].
	t check: 'an inlined loop computes correctly, hot' equals: 0 is: [ bad ].

	"--- a loop that allocates, so a scavenge can land on the back edge ---"
	bad := 0.
	1 to: 800 do: [:i |
		(d drive: 30) = 30 ifFalse: [bad := bad + 1]].
	t check: 'an inlined ALLOCATING loop survives collections' equals: 0 is: [ bad ].

	"--- to:do: plus a callee that falls off the end ---"
	bad := 0.
	1 to: hot do: [:i | | inst |
		inst := InlineCfE new init.
		(inst drive: 4) == inst ifFalse: [bad := bad + 1].
		inst count = 10 ifFalse: [bad := bad + 1]].
	t check: 'a fall-off-the-end callee with a loop answers self and has run'
		equals: 0 is: [ bad ].

	"--- several non-tail returns ---"
	bad := 0.
	1 to: hot do: [:i |
		(f drive: 1) == #one ifFalse: [bad := bad + 1].
		(f drive: 2) == #two ifFalse: [bad := bad + 1].
		(f drive: 3) == #three ifFalse: [bad := bad + 1].
		(f drive: 9) == #other ifFalse: [bad := bad + 1]].
	t check: 'each of four returns leaves the inlined body with its own value'
		equals: 0 is: [ bad ].

	"--- guard failure AFTER the site is hot and inlined ---"
	bad := 0.
	1 to: hot do: [:i | (a drive: 50) == #big ifFalse: [bad := bad + 1]].
	1 to: 500 do: [:i |
		(sub drive: 50) == #overridden ifFalse: [bad := bad + 1].
		(a drive: 50) == #big ifFalse: [bad := bad + 1]].
	t check: 'a guard miss falls back to the real send, the hit keeps inlining'
		equals: 0 is: [ bad ].

	"--- an exception raised from inside an inlined body with control flow ---"
	bad := 0.
	1 to: hot do: [:i | (g drive: 2) = 4 ifFalse: [bad := bad + 1]].
	1 to: 200 do: [:i |
		[ g drive: 99. bad := bad + 1 ]
			on: Error do: [:ex | ex messageText = 'too big' ifFalse: [bad := bad + 1]].
		(g drive: 2) = 4 ifFalse: [bad := bad + 1]].
	t check: 'an exception from an inlined body unwinds, and the site still works'
		equals: 0 is: [ bad ].

	"--- branching on an ivar: correct whether or not it is inlined ---"
	bad := 0.
	e := InlineIvarBranch new setFlag: true.
	1 to: hot do: [:i |
		e drive == #yes ifFalse: [bad := bad + 1]].
	e setFlag: false.
	1 to: 500 do: [:i |
		e drive == #no ifFalse: [bad := bad + 1]].
	t check: 'a callee branching on an instance variable stays correct'
		equals: 0 is: [ bad ].

	^t report
]
