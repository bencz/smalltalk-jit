"Kernel unit-test runner. Every check is wrapped so an unexpected runtime
 error counts as a FAILURE instead of killing the run half-way. A test file
 creates one instance (TestRun named: 'Suite'), drives check:/expect: on it,
 and its final top-level block MUST end with `^t report`: the returned fail
 count becomes the process exit code (0 = pass) that run_tests.sh gates on.
 The VM independently counts UNCAUGHT errors (any fiber) and forces a nonzero
 exit code for them, so neither a raw error nor a missing report can produce
 a silent false pass. All catches use Exception (the root) so a wrong
 error-class name surfaces as a visible FAIL, never a silent exit-0."
TestRun := Object [

	| fails count name |


	class named: aString [
		^self new setName: aString
	]


	setName: aString [
		name := aString.
		fails := 0.
		count := 0.
		^self
	]


	fail: aLabel [
		fails := fails + 1.
		Transcript nextPutAll: name; nextPutAll: ' FAIL: '; nextPutAll: aLabel; lf.
	]


	error: aLabel from: e [
		fails := fails + 1.
		Transcript nextPutAll: name; nextPutAll: ' ERROR ['; nextPutAll: aLabel;
			nextPutAll: ']: '; nextPutAll: e messageText; lf.
	]


	check: aLabel that: aBlock [
		count := count + 1.
		[ (aBlock value = true) ifFalse: [ self fail: aLabel ] ]
			on: Exception do: [:e | self error: aLabel from: e ].
	]


	check: aLabel equals: expected is: aBlock [
		count := count + 1.
		[ | got |
		  got := aBlock value.
		  got = expected ifFalse: [
			  self fail: aLabel, ' (got ', got printString, ' expected ', expected printString, ')' ] ]
			on: Exception do: [:e | self error: aLabel from: e ].
	]


	expect: aLabel raises: aClass in: aBlock [
		count := count + 1.
		[ aBlock value. self fail: aLabel, ' (no error raised)' ]
			on: Exception do: [:e |
				(aClass notNil and: [ e isKindOf: aClass ]) ifFalse: [
					self fail: aLabel, ' (raised ', e class printString, ' not ', aClass printString, ')' ] ].
	]


	report [
		Transcript nextPutAll: name; nextPutAll: ': ';
			nextPutAll: (count - fails) printString; nextPutAll: '/';
			nextPutAll: count printString; nextPutAll: ' ok'; lf.
		^fails
	]

]
