Block := Object [

	"A FLAT CLOSURE (ADR 0008), and the VM owns the layout: an element count,
	 the CompiledMethod holding the block's code, the token of the activation a
	 `^` inside it returns from, then the captured values.

	 There is no chain of contexts any more. A block captures what it reads BY
	 VALUE into itself, and a variable it also ASSIGNS gets a heap cell that the
	 closure captures the pointer of; reaching a captured value is one load at a
	 known offset rather than a walk up a chain of activations. That is what
	 makes `receiver`, `outerContext` and `homeContext` disappear from here:
	 the first is a capture like any other, and the other two named links in a
	 chain that no longer exists.

	 The price, accepted with open eyes in the ADR: reflection over an
	 activation is no longer free. It MATERIALISES a Context from the native
	 frame, which is a primitive rather than a field read."

	<shape: ClosureShape>


	"accessing"

	compiledBlock [
		^self notYetImplemented
	]


	receiver [
		"Captured by name when the block reads it, and not a field."
		^self notYetImplemented
	]


	outerContext [
		^self notYetImplemented
	]


	homeContext [
		^self notYetImplemented
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
