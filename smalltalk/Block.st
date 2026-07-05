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

	ensure: aBlock [
		"Evaluate the receiver and answer its value, evaluating aBlock afterwards
		whether the receiver completes normally or is unwound by an exception."
		| result |

		result := [self value] on: Exception do: [:ex | aBlock value. ex pass].
		aBlock value.
		^result
	]


	ifCurtailed: aBlock [
		"Evaluate aBlock only if the receiver is curtailed by an exception."
		^[self value] on: Exception do: [:ex | aBlock value. ex pass]
	]


	on: anExceptionClass do: aHandlerBlock [
		"Evaluate the receiver; if it signals an exception handled by
		anExceptionClass, evaluate aHandlerBlock with the exception. The handler
		may answer a value, or send #return:/#retry/#pass to the exception."
		^self basicOn: anExceptionClass do: [:ex |
			ex setRetryProtected: self class: anExceptionClass handler: aHandlerBlock.
			aHandlerBlock value: ex]
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
