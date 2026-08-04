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
		"THE LAST RESORT, so nothing in here may raise.

		 defaultAction is what runs when NOBODY handled the exception, and an
		 exception raised inside it has nowhere left to go but defaultAction
		 again. Both statements below were exactly that: signalingContext answers
		 `self notYetImplemented` until the ADR 0008 materialiser exists, and
		 noteUnhandled was a primitive with an empty body, so each one raised and
		 re-entered here. Every unhandled error in the system died of stack
		 exhaustion instead of being reported, which is a SEGFAULT where the
		 design says print-and-exit-non-zero.

		 The backtrace is GUARDED rather than removed: it comes back for free the
		 day a Context can be materialised, and until then a missing backtrace is
		 worth strictly less than the message, which still prints."
		Transcript nextPutAll: self messageText; lf.
		[self signalingContext printBacktrace] on: Error do: [ :ignored | ].
		"Count this death so main folds it into a nonzero process exit code
		 (non-interactive runs): a test can no longer print a backtrace and
		 still report success. Resumable defaults (Warning) never reach here."
		self noteUnhandled.
		Processor thisProcess terminate.
	]


	noteUnhandled [
		<primitive: UnhandledErrorNotePrimitive>
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
		"The context the handler runs in, found by walking OUT of the Exception
		 frames that got here: every frame whose receiver is this exception is
		 one of its own methods (this one, generateBacktrace, defaultAction),
		 and the first frame beyond them belongs to whoever asked. The handler
		 runs on top of the signaling frames (nothing was unwound), so the walk
		 is over live frames.

		 thisContext is not a field read in jit-v2: an activation is a native
		 frame, and a Context is MATERIALISED from it on demand (ADR 0008).
		 nil when the walk runs out (the signal came straight from the VM)."
		| context |

		context := thisContext.
		[context notNil and: [context receiver == self]]
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
