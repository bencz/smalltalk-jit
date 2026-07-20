"Internal control machinery of the resumable exception protocol. NOT part of
 the public surface.

 While an exception handler runs (on top of the signaling frames, see
 Exception>>runHandledBy:), resume:/return:/retry/retryUsing:/resignalAs: must
 escape the handler from anywhere inside it. The escape is itself a signal of
 HandlerEscape, caught by the wrapper each runHandledBy: installs. HandlerEscape
 deliberately subclasses Object, NOT Exception, so no user handler (`on:
 Exception do:`) can ever intercept it; it carries just enough of the signaling
 protocol (signal + class handles: + runHandledBy: + defaultAction) for the VM
 machinery to dispatch it, and its own handlers never escape, so the protocol
 bottoms out here."

HandlerEscape := Object [

	| target unwind value |


	class handles: anException [
		^anException isKindOf: self
	]


	class target: anException unwind: aBoolean value: aValue [
		^self new setTarget: anException unwind: aBoolean value: aValue
	]


	setTarget: anException unwind: aBoolean value: aValue [
		target := anException.
		unwind := aBoolean.
		value := aValue.
	]


	target [
		^target
	]


	"The (unwind -> value) decision this escape carries, in the Association
	 shape the ExceptionSignal primitive interprets."
	response [
		^unwind -> value
	]


	signal [
		<primitive: ExceptionSignal>
		^self defaultAction
	]


	"The signal primitive's hook. An escape's own handler (the runHandledBy:
	 wrapper) always completes normally, answering the response for the
	 activation being escaped, so this is the plain no-wrapper form."
	runHandledBy: aHandlerBlock [
		^true -> (aHandlerBlock value: self)
	]


	defaultAction [
		^Error signal: 'exception escape outside of an active handler (resume:/return:/retry sent after the handler completed?)'
	]

]


"Answered through basicOn:do: to make Block>>on:do: re-run the protected block
 (retry) or a replacement (retryUsing:). Created only by Exception>>retry and
 Exception>>retryUsing:, after the stack was already unwound to the on:do:."
RetryRequest := Object [

	| block |


	class block: aBlockOrNil [
		^self new setBlock: aBlockOrNil
	]


	setBlock: aBlockOrNil [
		block := aBlockOrNil.
	]


	block [
		^block
	]

]


"Resumed into the original basicSignal to make Exception>>signal re-signal a
 replacement exception from the ORIGINAL signal environment (resignalAs:)."
ResignalRequest := Object [

	| exception |


	class exception: anException [
		^self new setException: anException
	]


	setException: anException [
		exception := anException.
	]


	exception [
		^exception
	]

]
