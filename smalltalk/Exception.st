Exception := Object [

	| messageText |


	"instance creation"

	class signal [
		^self new signal
	]


	class signal: aString [
		^self new
			messageText: aString;
			signal
	]


	"exception handling"

	class handles: anException [
		^anException isKindOf: self
	]


	"The raw VM signal. When a matching on:do: handler exists, the VM runs it
	 on top of THESE frames (no unwinding) through runHandledBy: below, and
	 this primitive answers either the handler's resumption value (execution
	 simply continues here) or, after unwinding to the on:do: frame, makes the
	 on:do: answer the handler's value (in which case control never returns
	 here). With no handler the default action runs; if it answers (a
	 resumable default, e.g. Warning), that answer becomes the value of the
	 signal expression."
	basicSignal [
		<primitive: ExceptionSignal>
		^self defaultAction
	]


	signal [
		| result |
		result := self basicSignal.
		"resignalAs: resumes the original signal with a request to re-signal a
		 replacement from this same environment (the full handler chain was
		 restored by the resumption, so even the handler that resignaled is
		 eligible again)"
		result class == ResignalRequest ifTrue: [^result exception signal].
		^result
	]


	signal: aString [
		messageText := aString.
		^self signal
	]


	"The VM's handler-invocation hook (ExceptionSignal primitive): runs the
	 handler block on top of the signaling frames and answers the decision as
	 an Association: true -> value unwinds to the on:do: which answers value;
	 false -> value resumes the signal expression with value. The HandlerEscape
	 wrapper is what lets resume:/return:/retry/retryUsing:/resignalAs: exit
	 the handler from anywhere inside it; an escape aimed at an OUTER
	 activation is re-signaled so it keeps searching outward, abandoning this
	 activation when the outer one unwinds."
	runHandledBy: aHandlerBlock [
		^[true -> (aHandlerBlock value: self)]
			on: HandlerEscape
			do: [:escape |
				escape target == self
					ifTrue: [escape response]
					ifFalse: [escape signal]]
	]


	"private: leave the running handler from anywhere inside it"
	escapeUnwind: aBoolean with: aValue [
		(HandlerEscape target: self unwind: aBoolean value: aValue) signal.
	]


	"Answer aValue as the result of the protected on:do: block, unwinding the
	 signaling frames (their pending ensure:/ifCurtailed: cleanups run)."
	return: aValue [
		self escapeUnwind: true with: aValue.
	]


	return [
		self return: nil.
	]


	"Answer aValue as the value of the original signal expression and continue
	 execution from the signal point. No unwinding happens: the pending
	 ensure:/ifCurtailed: registrations between the signal and the on:do: stay
	 armed. Only legal for resumable exceptions."
	resume: aValue [
		self isResumable ifFalse: [^self error: 'attempt to resume a non-resumable exception'].
		self escapeUnwind: false with: aValue.
	]


	resume [
		^self resume: nil
	]


	"Unwind to the on:do:, then re-evaluate its protected block from the start."
	retry [
		self escapeUnwind: true with: (RetryRequest block: nil).
	]


	"Unwind to the on:do:, then evaluate aBlock in place of its protected block."
	retryUsing: aBlock [
		self escapeUnwind: true with: (RetryRequest block: aBlock).
	]


	"Signal replacementException from the ORIGINAL signal environment, as if it
	 had been signaled there in the first place: the handler search starts over
	 and includes the currently running handler."
	resignalAs: replacementException [
		self escapeUnwind: false with: (ResignalRequest exception: replacementException).
	]


	"Evaluate the enclosing handler for this exception. If it resumes, its
	 resumption value is answered HERE and the current handler continues; if it
	 unwinds, this activation is abandoned with it."
	outer [
		^self signal
	]


	"Yield the decision to the enclosing handler outright. If it resumes, the
	 value goes to the ORIGINAL signal expression and the current handler is
	 abandoned; an unwinding decision cuts past this activation anyway."
	pass [
		self resume: self outer.
	]


	isResumable [
		^true
	]


	"Default action for an unhandled exception, running on top of the signaling
	 frames (nothing was unwound to get here). This base version prints and
	 terminates the fiber; terminate runs the fiber's pending ensure:/
	 ifCurtailed: cleanups on its own stack first. A worker fiber dying this
	 way is invisible to its peers: the per-mutator handler chain heads stay
	 published (schedExceptionHandler/schedUnwindHandler), so a cross-thread GC
	 keeps scanning every other fiber's chains as before. Resumable subclasses
	 (e.g. Warning) override this to ANSWER a value instead, which becomes the
	 value of the signal expression and execution continues at the signal
	 point."
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
