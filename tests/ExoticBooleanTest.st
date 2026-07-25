"The invariant that the boolean fast paths rest on, which nothing else covers.

 An inlined conditional asks 'is the receiver an INSTANCE OF True', and the JIT
 answers it by first comparing the receiver against the `true` and `false`
 singletons, because that is two instructions instead of a class dispatch. That
 shortcut is only sound because it REORDERS rather than REPLACES: a value that
 is neither singleton still goes through the real class check.

 `True new` is exactly that value. It is a second, distinct instance of class
 True, so a pointer comparison against the `true` singleton says 'no' while the
 class check says 'yes', and the two must not disagree. Nothing in the suite
 built one before, which meant the generic arm of both the singleton fast path
 (Phase 1a) and the fused three-way branch (Phase 1b) ran in no test at all:
 deliberately breaking the complementary-class comparison inside it left all 193
 tests green.

 Covers every shape the compiler inlines: ifTrue:, ifFalse:, ifTrue:ifFalse:,
 and:, or:, whileTrue:. Worth running under ST_NO_FUSE_BOOL=1 too, which turns
 the singleton compares off and must answer identically.

 Harness note (see BooleanGlobalTest.st): an uncaught error still exits 0, so
 this test counts failures and ^-returns the count."

ExoticBoolProbe := Object [
	bothArms: b   [ ^b ifTrue: ['T'] ifFalse: ['F'] ]
	trueOnly: b   [ b ifTrue: [ ^'T' ]. ^'fell' ]
	falseOnly: b  [ b ifFalse: [ ^'F' ]. ^'fell' ]
	andWith: b    [ ^b and: [ true ] ]
	orWith: b     [ ^b or: [ false ] ]

	"whileTrue: over an exotic boolean: the condition is re-read every
	 iteration, so the check has to keep answering the same way."
	countDown: b [
		| n flag |
		n := 0.
		flag := b.
		[ flag ] whileTrue: [
			n := n + 1.
			n >= 3 ifTrue: [ flag := false ] ].
		^n
	]
]

[
	| fails p exoticTrue exoticFalse |
	fails := 0.
	p := ExoticBoolProbe new.
	exoticTrue := True new.
	exoticFalse := False new.

	"Precondition: these really are distinct objects of the right classes.
	 If a future change makes True new answer the singleton, the rest of this
	 test would still pass while covering nothing, so assert it explicitly."
	(exoticTrue class == True) ifFalse: [ fails := fails + 1 ].
	(exoticFalse class == False) ifFalse: [ fails := fails + 1 ].
	(exoticTrue == true) ifTrue: [ fails := fails + 1 ].
	(exoticFalse == false) ifTrue: [ fails := fails + 1 ].

	"An instance of True behaves as true in every inlined shape, and an
	 instance of False as false."
	([ (p bothArms: exoticTrue) = 'T' ]  on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p bothArms: exoticFalse) = 'F' ] on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p trueOnly: exoticTrue) = 'T' ]  on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p trueOnly: exoticFalse) = 'fell' ] on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p falseOnly: exoticFalse) = 'F' ] on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p falseOnly: exoticTrue) = 'fell' ] on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p andWith: exoticTrue) = true ]   on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p andWith: exoticFalse) = false ] on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p orWith: exoticFalse) = false ]  on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p orWith: exoticTrue) = true ]    on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p countDown: exoticTrue) = 3 ]    on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ (p countDown: exoticFalse) = 0 ]   on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].

	"And a NON boolean must still raise, from the same fused branch: that is the
	 third way out of the three-way test, and skipping it would silently pick an
	 arm for nil."
	([ p bothArms: nil. false ] on: Error do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].
	([ p bothArms: 7. false ]   on: Error do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].

	fails > 0 ifTrue: [
		Transcript nextPutAll: 'ExoticBooleanTest FAILURES='; nextPutAll: fails printString; lf ].
	^fails
]
