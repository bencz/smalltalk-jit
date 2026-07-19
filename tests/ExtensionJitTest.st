"Class extension under hot JIT'd call sites. The extend commit swaps the
 method dictionaries inside the same stop-the-world bracket that resets
 every IC cell and flushes the lookup caches, so the very next iteration of
 a tiered loop must observe the extension. Two hazards are driven hot here:
 an INHERITED override (IC cells bound to the superclass method for this
 receiver class must rebind) and DNU-then-define (DNU stub bindings baked
 into caches must die with the definition)."

ExtJitBase := Object [
	probe [ ^1 ]
]

ExtJitSub := ExtJitBase [
]

ExtJitDnu := Object [
]

ExtJitDriver := Object [

	overrides: aTestRun [
		| before after |
		before := 0.
		after := 0.
		1 to: 300000 do: [ :i |
			i <= 150000
				ifTrue: [ before := before + ExtJitSub new probe ]
				ifFalse: [ after := after + ExtJitSub new probe ].
			i = 150000 ifTrue: [ ExtJitDriver extendOverride ] ].
		aTestRun check: 'hot inherited sends dispatch the superclass method before the extension'
			equals: 150000 is: [ before ].
		aTestRun check: 'every send after the extension sees the override'
			equals: 300000 is: [ after ].
	]

	"NOTE: the caught-DNU volume stays below ~20000 on purpose: a
	 PRE-EXISTING leak in the hot caught-DNU path (present without any
	 extension, on the pre-package baseline) overflows the fiber stack past
	 ~30000 caught MessageNotUnderstood in one method. Tracked separately."
	dnu: aTestRun [
		| misses hits |
		misses := 0.
		hits := 0.
		1 to: 16000 do: [ :i |
			| r |
			r := [ ExtJitDnu new dnuProbe ] on: MessageNotUnderstood do: [ :e | #miss ].
			r == #miss ifTrue: [ misses := misses + 1 ].
			r == #defined ifTrue: [ hits := hits + 1 ].
			i = 8000 ifTrue: [ ExtJitDriver extendDnu ] ].
		aTestRun check: 'every send before the definition raised MessageNotUnderstood'
			equals: 8000 is: [ misses ].
		aTestRun check: 'every send after the definition finds the new method (DNU stubs died)'
			equals: 8000 is: [ hits ].
	]

	class extendOverride [
		Compiler new buildClass:
			(Parser parseString: 'ExtJitSub extend [ probe [ ^2 ] ]') parseClass.
	]

	class extendDnu [
		Compiler new buildClass:
			(Parser parseString: 'ExtJitDnu extend [ dnuProbe [ ^#defined ] ]') parseClass.
	]

]

[ | t d |
	t := TestRun named: 'ExtensionJit'.
	d := ExtJitDriver new.
	d overrides: t.
	d dnu: t.
	^t report
]
