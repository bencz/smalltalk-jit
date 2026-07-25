"COMPILE-TIME DEVIRTUALIZATION staleness (jit/x64/CodeGeneratorX64.c
 generateSend, core/CompiledCode.h compiledCodeResolveOperandClass).

 When compiledCodeResolveOperandClass answers an exact class for a send's
 receiver, generateSend bakes lookupNativeCode(class, selector) as an
 immediate and emits NO class guard and NO IC cell at all. That happens for
 super sends, literal receivers, nil/true/false and blocks. There is nothing
 for icInvalidateAllSends to reset, so a later extend or redefinition of the
 resolved class leaves the baked entry pointing at the REPLACED method.

 This variant is INDEPENDENT of the tier: it lives in ordinary tier-0 code and
 reproduces without the counter ever firing. It must therefore also fail under
 ST_NO_TIER=1, which is what separates it from the other two variants."

SuperStaleBase := Object [
	greet [ ^1 ]
]

SuperStaleSub := SuperStaleBase [
	"the super send is devirtualized at compile time, unguarded"
	greet [ ^super greet + 10 ]
]

SuperStaleDriver := Object [

	class redefineBaseTo: aString [
		Compiler new buildClass:
			(Parser parseString: 'SuperStaleBase := Object [ greet [ ^', aString, ' ] ]') parseClass.
	]

]

[ | t |
	t := TestRun named: 'SuperExtendStale'.

	"Force SuperStaleSub>>greet to be COMPILED, which bakes the super entry.
	 Compilation is lazy per method, so one call is enough."
	t check: 'super send answers the original base method' equals: 11 is: [ SuperStaleSub new greet ].

	SuperStaleDriver redefineBaseTo: '2'.

	t check: 'a direct send to the base sees the redefinition' equals: 2 is: [ SuperStaleBase new greet ].
	t check: 'the compiled SUPER send sees the redefinition' equals: 12 is: [ SuperStaleSub new greet ].

	SuperStaleDriver redefineBaseTo: '5'.
	t check: 'the super send tracks a SECOND redefinition' equals: 15 is: [ SuperStaleSub new greet ].

	^t report
]
