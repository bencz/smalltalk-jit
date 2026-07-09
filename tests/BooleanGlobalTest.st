"Regression (Bug A): a boolean stored in a GLOBAL variable must work as the
 receiver of an inlined conditional (ifTrue:/ifFalse:/ifTrue:ifFalse:/and:/or:)
 inside a method. The inlined class-check used to constant-fold on the
 Association object's class (always Association, never True/False), so EVERY
 global receiver funnelled straight to #mustBeBoolean regardless of its value.
 Size-independent: it does not depend on the system dictionary growing.

 NOTE on the harness: in this VM an uncaught error (or an Assert failure) prints
 a backtrace but the process still exits 0 (defaultAction terminates the fiber;
 main returns EXIT_SUCCESS). run_tests.sh only flags a test whose LAST block
 ^-returns a nonzero integer. So this test counts failures and returns the count
 — a caret return of 0 means all good; nonzero means a regression it can catch.
 Bug A raises, so each check is wrapped in `on: Error do:` to turn a raise into a
 counted failure instead of a silent fiber kill. Globals are auto-created when
 referenced by the methods below; their values are set inside the block (a bare
 top-level `Name := x.` would be a parse error here)."

BoolGlobProbe := Object [
	whenTrue  [ BoolGlobFlag ifTrue: [ ^'T' ]. ^'F' ]
	whenFalse [ BoolGlobOff ifFalse: [ ^'wasFalse' ]. ^'wasTrue' ]
	bothArms  [ BoolGlobFlag ifTrue: [ ^'yes' ] ifFalse: [ ^'no' ] ]
	andChain  [ ^(BoolGlobFlag and: [ true ]) ]
	orChain   [ ^(BoolGlobOff or: [ false ]) ]
]

NilGlobProbe := Object [
	check [ NilGlob ifTrue: [ ^'oops' ]. ^'unreached' ]
]

[
	| fails p |
	fails := 0.
	BoolGlobFlag := true.
	BoolGlobOff  := false.
	NilGlob      := nil.
	p := BoolGlobProbe new.

	([ p whenTrue = 'T' ]         on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ p whenFalse = 'wasFalse' ] on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ p bothArms = 'yes' ]       on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ p andChain = true ]        on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].
	([ p orChain = false ]        on: Error do: [ :e | false ]) ifFalse: [ fails := fails + 1 ].

	"a nil global must STILL raise from an inlined conditional (nil isn't boolean)"
	([ NilGlobProbe new check. false ] on: Error do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].

	fails > 0 ifTrue: [
		Transcript nextPutAll: 'BooleanGlobalTest FAILURES='; nextPutAll: fails printString; lf ].
	^fails
]
