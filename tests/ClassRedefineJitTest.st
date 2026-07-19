"Class REDEFINITION under hot JIT'd call sites. IC cells are keyed by the
 class OBJECT, whose identity survives a redefinition (the new raw class is
 copied over the old one in place), so without install-time invalidation a
 hot send kept dispatching into the REPLACED method dictionary's code until
 the next scavenge happened to reset the cells. This drives a send site past
 the tier threshold, redefines the class mid-loop through the in-image
 compiler, and requires the very next iteration to see the new method."

RedefHot := Object [
	probe [ ^1 ]
]

RedefDriver := Object [

	run: aTestRun [
		| before after |
		before := 0.
		after := 0.
		1 to: 300000 do: [ :i |
			i <= 150000
				ifTrue: [ before := before + RedefHot new probe ]
				ifFalse: [ after := after + RedefHot new probe ].
			i = 150000 ifTrue: [ RedefDriver redefine ] ].
		aTestRun check: 'hot sends before redefinition dispatch the old method'
			equals: 150000 is: [ before ].
		aTestRun check: 'every send after redefinition sees the new method'
			equals: 300000 is: [ after ].
	]

	class redefine [
		Compiler new buildClass:
			(Parser parseString: 'RedefHot := Object [ probe [ ^2 ] ]') parseClass.
	]

]

[ | t |
	t := TestRun named: 'ClassRedefineJit'.
	RedefDriver new run: t.
	^t report
]
