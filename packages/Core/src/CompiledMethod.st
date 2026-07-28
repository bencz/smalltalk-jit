CompiledMethod := CompiledCode [

	"The two TAGGED fields of RawCompiledMethod, in order. `literals` and
	 `descriptors` are not here any more: the literal frame belongs to the
	 CodeUnit and the bci-to-machine map is an array beside the native code, both
	 of them C structures the collector reaches through jit/Jit.c rather than
	 through this object."

	| selector ownerClass |


	"accessing"

	literals [
		"The literal frame belongs to the CodeUnit now, not to this object."
		^self notYetImplemented
	]


	selector [
		^selector
	]


	ownerClass [
		^ownerClass
	]


	sourceCode [
		^self notYetImplemented
	]


	descriptors [
		"An array beside the native code in jit-v2, not a field here."
		^self notYetImplemented
	]


	sendTo: anObject [
		<primitive: MethodSendPrimitive>
	]


	sendTo: anObject withArguments: anArray [
		<primitive: MethodSendArgsPrimitive>
		Error signal.
	]


	"printing"

	printOn: aStream [
		ownerClass basicPrintOn: aStream.
		selector printOn: aStream.
	]

]
