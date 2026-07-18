"Exercises the extended exception protocol: signal:, messageText, description,
return:, pass, ensure:, ifCurtailed: and retry. Split into small blocks to stay
under the compiler's large-method limit. All blocks share one TestRun through
the global EPRun; the last block returns the report."

[
	"signalling and message text"
	| t |
	t := TestRun named: 'ExceptionProtocol'.
	EPRun := t.
	t check: 'signal: on instance sets messageText' equals: 'boom' is: [
		[Error new signal: 'boom'] on: Error do: [:e | e messageText] ].
	t check: 'signal: on class sets messageText' equals: 'klass' is: [
		[Error signal: 'klass'] on: Error do: [:e | e messageText] ].
	t check: 'description defaults to messageText' equals: 'd' is: [
		[Error new signal: 'd'] on: Error do: [:e | e description] ].
	t check: 'handler sees the signaled class' that: [
		([Error signal] on: Error do: [:e | e class]) == Error ].
	t check: 'no signal answers protected value' equals: 42 is: [
		[42] on: Error do: [:e | 99] ].
	t check: 'handler value becomes on:do: value' equals: 7 is: [
		[Error signal] on: Error do: [:e | 7] ].
]


[
	"return: answers the value of the protected on:do:"
	| t |
	t := EPRun.
	t check: 'return: answers the given value' equals: 123 is: [
		[Error signal] on: Error do: [:e | e return: 123] ].
	t check: 'return answers nil' that: [
		([Error signal] on: Error do: [:e | e return]) isNil ].
	t check: 'return: value usable in arithmetic' equals: 6 is: [
		([Error signal: 'x'] on: Error do: [:e | e return: 5]) + 1 ].
]


[
	"pass re-signals to the enclosing handler"
	| t |
	t := EPRun.
	t check: 'pass reaches the outer handler' equals: 'outer' is: [
		[[Error signal] on: Error do: [:e | e pass]] on: Error do: [:e | 'outer'] ].
	t check: 'pass preserves messageText' equals: 'orig' is: [
		[[Error new signal: 'orig'] on: Error do: [:e | e pass]]
			on: Error do: [:e | e messageText] ].
	"an unhandled inner class propagates to the matching outer handler"
	t check: 'unmatched inner class propagates outward' equals: 2 is: [
		[[Error signal] on: MessageNotUnderstood do: [:e | 1]]
			on: Error do: [:e | 2] ].
]


[
	"ensure: runs the cleanup on normal completion, answering the receiver value"
	| t log |
	t := EPRun.
	log := OrderedCollection new.
	t check: 'ensure: answers the protected value' equals: 42 is: [
		[log add: 1. 42] ensure: [log add: 2] ].
	t check: 'ensure: protected block ran first' equals: 1 is: [ log first ].
	t check: 'ensure: cleanup ran last' equals: 2 is: [ log last ].

	"ensure: runs the cleanup while unwinding through an exception"
	log := OrderedCollection new.
	t check: 'ensure: during exception answers handler value' equals: #caught is: [
		[[Error signal] ensure: [log add: #cleanup]] on: Error do: [:e | #caught] ].
	t check: 'ensure: cleanup ran during unwind' that: [ log includes: #cleanup ].
]


[
	"ifCurtailed: runs only when curtailed"
	| t log |
	t := EPRun.
	log := OrderedCollection new.
	t check: 'ifCurtailed: answers value on normal completion' equals: 100 is: [
		[100] ifCurtailed: [log add: #ran] ].
	t check: 'ifCurtailed: block skipped on normal completion' that: [ log isEmpty ].

	log := OrderedCollection new.
	([[Error signal] ifCurtailed: [log add: #ran]] on: Error do: [:e | nil]).
	t check: 'ifCurtailed: ran when curtailed' that: [ log includes: #ran ].
]


[
	"retry re-evaluates the protected block"
	| t attempts result |
	t := EPRun.
	attempts := OrderedCollection new.
	result := [attempts add: 1.
		attempts size < 3 ifTrue: [Error signal: 'again'].
		attempts size]
			on: Error do: [:e | e retry].
	t check: 'retry re-evaluates until success' equals: 3 is: [ result ].
	t check: 'retry ran the expected number of attempts' equals: 3 is: [ attempts size ].

	"resume is not supported on Error and fails clearly rather than silently"
	t expect: 'resume: on Error raises' raises: Error
		in: [[Error signal] on: Error do: [:e | e resume: 1]].
]


[
	^EPRun report
]
