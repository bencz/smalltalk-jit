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
	 struct and not a heap object (vm/compiler/Bytecode.h), so each of these is
	 a primitive where v1 had a field read. They accept a method or a block:
	 the two are the same object with different classes and every question here
	 is about the unit."

	argumentsSize [
		<primitive: CodeArgumentCountPrimitive>
	]


	temporariesSize [
		"DERIVED FROM THE SOURCE, like `temporaries` and `arguments` below, and
		 not from the register count: the frame also holds `self`, the captures
		 and whatever scratch registers the emitter needed, and a method with no
		 temporaries at all has several. What the question means is how many the
		 programmer DECLARED."
		^self temporaries size
	]


	hasContext [
		<primitive: CodeHasContextPrimitive>
	]


	outerReturns [
		<primitive: CodeOuterReturnsPrimitive>
	]


	primitive [
		"The primitive NUMBER, or nil for a method that has none."
		<primitive: CodePrimitiveNumberPrimitive>
	]


	literals [
		<primitive: CodeLiteralsPrimitive>
	]


	descriptors [
		<primitive: CodeDescriptorsPrimitive>
	]


	sourceCode [
		"WHERE the text is, not the text: a SourceCode names a file (or a
		 string) plus a position and a length, and `sourceContents` is what
		 fetches it. That is what makes reflection over a method possible
		 without keeping a parse tree alive."
		<primitive: CodeSourcePrimitive>
	]


	header [
		"THE OLD PACKED WORD, REBUILT from the parts that replaced it, because
		 the image still asks for it. It is a DERIVED value now and not a field:
		 argument count in the low byte, declared temporaries in the next, the
		 primitive number above that, and the hasContext flag at the top. Zero
		 for a method that takes nothing, declares nothing, has no primitive and
		 is nobody's home, which is what the trivial case meant in v1 too."
		| bits |

		bits := self argumentsSize.
		bits := bits bitOr: (self temporariesSize bitShift: 8).
		self primitive ifNotNil: [ :number |
			bits := bits bitOr: (number bitShift: 16) ].
		self hasContext ifTrue: [ bits := bits bitOr: (1 bitShift: 30) ].
		^bits
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
