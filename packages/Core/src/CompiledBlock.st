CompiledBlock := CompiledCode [

	"The code of a block that is NOT inlined. It is the same object a method's
	 code is (vm/jit/CompiledMethod.h) with a different class, and the class is
	 what buys the two things a block can be asked and a method cannot: which
	 method it was written in, and that it should print as one.

	 The two fields mirror CompiledMethod's, and they are DECLARED rather than
	 inherited because the shape has to match the object the VM builds: a class
	 whose instances carry two tagged words while its shape says none would have
	 those two words unscanned by the collector."

	| selector ownerClass |


	"accessing"

	method [
		"The CompiledMethod this block was written inside, or nil for a block
		 compiled outside any method (a top-level one in a script). A block
		 nested in another block answers the same METHOD, not the block around
		 it, exactly as a non-local return does."
		<primitive: CodeHomeMethodPrimitive>
	]


	selector [
		"The enclosing method's selector: a block is named after where it was
		 written, which is what a backtrace and printString both want."
		^selector
	]


	ownerClass [
		"The class the block was written in. A top-level block has none, and
		 answers UndefinedObject rather than nil so that the answer is always a
		 class."
		^ownerClass isNil ifTrue: [ UndefinedObject ] ifFalse: [ ownerClass ]
	]


	literals [
		"THE METHOD'S frame, not a second one. A block's unit has literals of
		 its own, but what the image means by `aBlock literals` is the frame the
		 whole method shares, and answering the block's would make
		 `aBlock literals == aBlock method literals` false for no reason a
		 reader could act on."
		| home |

		home := self method.
		^home isNil ifTrue: [ super literals ] ifFalse: [ home literals ]
	]


	"printing"

	printOn: aStream [
		self ownerClass basicPrintOn: aStream.
		selector printOn: aStream.
		aStream nextPutAll: '[]'.
	]

]
