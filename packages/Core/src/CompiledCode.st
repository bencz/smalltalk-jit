CompiledCode := Object [

	"THE VM OWNS THIS LAYOUT (vm/jit/CompiledMethod.h). In jit-v2 a compiled
	 method is three RAW words the collector must not follow, holding the
	 bytecode size, the CodeUnit and the machine code, followed by the tagged
	 fields declared in CompiledMethod. The old `header` word is gone: what it
	 packed (argument and temporary counts, primitive number) lives in the
	 CodeUnit, which is a C struct rather than a heap object."

	<shape: CompiledMethodShape>


	"accessing"

	"WHAT THE OLD `header` WORD PACKED now lives in the CodeUnit, which is a C
	 struct and not a heap object (vm/compiler/Bytecode.h): argument count,
	 register count and primitive number. Reaching them from Smalltalk needs a
	 primitive apiece, and until one exists these say so rather than answering a
	 plausible zero."

	argumentsSize [
		^self notYetImplemented
	]


	temporariesSize [
		^self notYetImplemented
	]


	hasContext [
		"Every activation can materialise a Context on demand in jit-v2
		 (ADR 0008), so no method is marked as needing one."
		^false
	]


	outerReturns [
		^self notYetImplemented
	]


	primitive [
		^self notYetImplemented
	]


	sourceNode [
		^(Parser parseString: self sourceCode sourceContents) parseMethodOrBlock
	]


	temporaries [
		^self sourceNode temporaries collect: [ :temporary | temporary name]
	]


	arguments [
		^self sourceNode arguments collect: [ :argument | argument name]
	]



	"printing"

	"THE DISASSEMBLER IS GONE, and not by accident: it decoded the v1 bytecode,
	 instruction by instruction, and jit-v2's is a different language (fixed
	 8-byte, register based, three address, docs/jit-v2/04-bytecode.md). Printing
	 one of those lives in C for now (codeUnitPrint in vm/compiler/Compile.c);
	 a Smalltalk one comes back when something needs it, written against the
	 bytecode that actually exists."

]
