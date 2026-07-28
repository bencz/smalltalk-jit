CompiledBlock := CompiledCode [

	"A block's code is an ordinary CompiledMethod in jit-v2 (ADR 0008): a
	 closure holds one, and entering the block enters it like any other. This
	 class survives as the name the image uses for that role and declares no
	 fields of its own, because the VM layout it would mirror no longer exists."



	"accessing"

	method [
		^self notYetImplemented
	]


	sourceCode [
		^self notYetImplemented
	]


	descriptors [
		^self notYetImplemented
	]


	literals [
		^self notYetImplemented
	]


	ownerClass [
		^self notYetImplemented
	]


	"printing"

	printOn: aStream [
		aStream nextPutAll: 'a block'.
	]

]
