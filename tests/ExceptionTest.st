ExceptionTest := Object [

	class outerReturnWithinHandleBlock [
		[Exception signal] on: Exception do: [ :e | ^e].
	]


	class outerReturnWithinBlock [
		[^1] on: Exception do: [ :e | ].
	]


	class outerReturnWithinBlockOfBlock [
		[[^1] value] on: Exception do: [ :e | ].
	]

]


[
	| t exception |
	t := TestRun named: 'Exception'.
	ExceptionRun := t.

	exception := Exception new.
	t check: 'handler receives the signaled instance' that: [
		[exception signal] on: Exception do: [ :e | e == exception ] ].
	t check: 'Error signal delivers a different instance' that: [
		[Error signal] on: Exception do: [ :e | e ~= exception ] ].
]


[
	| t |
	t := ExceptionRun.
	t check: 'outer return within handler block answers the exception' that: [
		ExceptionTest outerReturnWithinHandleBlock isMemberOf: Exception ].
	t check: 'outer return within protected block' equals: 1 is: [
		ExceptionTest outerReturnWithinBlock ].
	t check: 'outer return within block of block' equals: 1 is: [
		ExceptionTest outerReturnWithinBlockOfBlock ].
]


[
	^ExceptionRun report
]
