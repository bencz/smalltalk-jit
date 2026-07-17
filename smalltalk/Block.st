Block := Object [

	<shape: BlockShape>

	| compiledBlock receiver outerContext homeContext |


	"accessing"

	compiledBlock [
		^compiledBlock
	]


	receiver [
		^receiver
	]


	outerContext [
		^outerContext
	]


	homeContext [
		^homeContext
	]


	"evaluating"

	value [
		<primitive: BlockValuePrimitive>
		OutOfRangeError signal.
	]


	value: arg [
		<primitive: BlockValuePrimitive1>
		OutOfRangeError signal.
	]


	value: arg1 value: arg2 [
		<primitive: BlockValuePrimitive2>
		OutOfRangeError signal.
	]


	value: arg1 value: arg2 value: arg3 [
		<primitive: BlockValuePrimitive3>
		OutOfRangeError signal.
	]


	valueWithArguments: anArray [
		<primitive: BlockValueArgsPrimitive>
		OutOfRangeError signal.
	]


	"controlling"

	whileTrue: aBlock [
		<primitive: BlockWhileTrue2Primitive>
		Error signal: self printString, ' must return a ', Boolean printString.
	]


	whileFalse: aBlock [
		[self value not] whileTrue: aBlock.
	]


	whileTrue [
		<primitive: BlockWhileTruePrimitive>
		Error signal: self printString, ' must return a ', Boolean printString.
	]


	whileFalse [
		[self value not] whileTrue.
	]


	repeat [
		[self value. true] whileTrue.
	]


	"guaranteed evaluation"

	valueUnwindProtected: aBlock [
		"Evaluate the receiver with aBlock registered as a pending unwind cleanup
		on this fiber's chain. If any unwind cuts through this frame (an exception
		unwinding to an outer handler, a non-local return, or Process terminate),
		the unwinder runs aBlock during the unwind, innermost-first; on normal
		completion nothing runs and the registration is unlinked."
		<primitive: BlockUnwindPrimitive>
	]


	ensure: aBlock [
		"Evaluate the receiver and answer its value; aBlock runs exactly once
		afterwards, whether the receiver completes normally or is unwound (by an
		exception, a non-local return, or termination)."
		| result |

		result := self valueUnwindProtected: aBlock.
		aBlock value.
		^result
	]


	ifCurtailed: aBlock [
		"Evaluate aBlock only if the receiver is curtailed (unwound by an
		exception, a non-local return, or termination)."
		^self valueUnwindProtected: aBlock
	]


	on: anExceptionClass do: aHandlerBlock [
		"Evaluate the receiver; if it signals an exception handled by
		anExceptionClass, evaluate aHandlerBlock with the exception ON TOP of
		the signaling frames (see Exception>>runHandledBy:). The handler may
		answer a value (unwinds and becomes this expression's value), or send
		#resume:/#return:/#retry/#retryUsing:/#pass/#outer/#resignalAs: to the
		exception. retry/retryUsing: arrive here as a RetryRequest after the
		unwind, and re-evaluate under the same protection."
		| result |

		result := self basicOn: anExceptionClass do: aHandlerBlock.
		result class == RetryRequest ifTrue: [
			^(result block isNil ifTrue: [self] ifFalse: [result block])
				on: anExceptionClass do: aHandlerBlock].
		^result
	]


	basicOn: anException do: aBlock [
		<primitive: BlockOnExceptionPrimitive>
	]


	on: anException do: aBlock on: anException2 do: aBlock2 [
		^[self on: anException do: aBlock] on: anException2 do: aBlock2.
	]


	on: anException do: aBlock on: anException2 do: aBlock2 on: anException3 do: aBlock3 [
		^[[self on: anException do: aBlock] on: anException2 do: aBlock2] on: anException3 do: aBlock3.
	]


	"schedulling"

	basicSpawn [
		<primitive: ProcessSpawnPrimitive>
	]


	newProcess [
		"Create a suspended process that will evaluate this block. Send #resume to start it."
		^Process fromId: self basicSpawn
	]


	fork [
		"Create and immediately schedule a process evaluating this block."
		| process |
		process := self newProcess.
		process resume.
		^process
	]

]
