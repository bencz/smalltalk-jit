Exception := Object [

	| messageText retryProtected retryClass retryHandler |


	"instance creation"

	class signal [
		self new signal.
	]


	class signal: aString [
		self new
			messageText: aString;
			signal.
	]


	"exception handling"

	class handles: anException [
		^anException isKindOf: self
	]


	signal [
		<primitive: ExceptionSignal>
		self defaultAction.
	]


	signal: aString [
		messageText := aString.
		^self signal
	]


	"Answer aValue as the result of the protected on:do: block. Intended as the
	last expression of a handler block, e.g. [:e | ... e return: aValue]."
	return: aValue [
		^aValue
	]


	return [
		^nil
	]


	"Re-signal the receiver to the next enclosing handler. While a handler block
	runs, the current handler is already its parent, so re-signalling searches
	outwards."
	pass [
		^self signal
	]


	outer [
		^self signal
	]


	"Retry support. Block>>on:do: records the protected block, exception class and
	handler here so a handler can re-run the protected block from the start."
	setRetryProtected: aBlock class: aClass handler: aHandler [
		retryProtected := aBlock.
		retryClass := aClass.
		retryHandler := aHandler.
	]


	retry [
		retryProtected isNil ifTrue: [^self error: 'retry outside of an exception handler'].
		^retryProtected on: retryClass do: retryHandler
	]


	"Resuming would require returning control to the point where the exception
	was signalled (a delimited continuation). This VM unwinds the stack between
	the signal and the handler, so resumption is not supported."
	resume: aValue [
		^self error: 'resumable exceptions are not supported'
	]


	resume [
		^self resume: nil
	]


	isResumable [
		^false
	]


	defaultAction [
		Transcript nextPutAll: self messageText; lf.
		self signalingContext printBacktrace.
		Processor thisProcess terminate.
	]


	"backtrace"

	generateBacktrace [
		| context backtrace |

		backtrace := OrderedCollection new: 8.
		context := self signalingContext.

		[backtrace add: context copy.
		context := context parent.
		context notNil]
			whileTrue.

		^backtrace
	]


	signalingContext [
		| context |

		context := thisContext.
		[(context code ownerClass == Exception or: [context code ownerClass inheritsFrom: Exception])
		or: [context code ownerClass == Exception class or: [context code ownerClass inheritsFrom: Exception class]]]
			whileTrue: [context := context parent].

		^context
	]


	"accessing"

	messageText: aString [
		messageText := aString.
	]


	messageText [
		^messageText isNil ifTrue: [self defaultMessageText] ifFalse: [messageText]
	]


	defaultMessageText [
		^self class name
	]


	description [
		^self messageText
	]

]
