"A dense array of UNBOXED 64-bit floats.

Elements are stored as native-endian IEEE-754 doubles directly in a byte
payload, with no per-element object header and no tag. Compared with an Array
holding Float objects that means:

  * no allocation per element, and no boxing when a computed value leaves the
    immediate SmallFloat64 range;
  * nothing for the garbage collector to scan. A one-million-element Array of
    Floats costs the collector a million scanned words on every cycle; a
    FloatArray of the same length costs it one header;
  * bulk operations become single C loops with no per-element message send,
    which is where the real difference is. `at:` is a convenience; `sum`,
    `dot:` and the elementwise operators are the reason this class exists.

WARNING: this is a 64-bit float array. Pharo's FloatArray is 32-bit and its
Float64Array is the 64-bit one; this VM has exactly one float type, the double,
so the plain name is used for it and there is no 32-bit variant.

WARNING on sizes: `basicSize` answers the BYTE count (8 per element) because
that is what the underlying shape stores, while `size` answers the ELEMENT
count. Inherited methods that reason in bytes (shallowCopy, deepCopy) are
correct precisely because they use basicSize and basicAt: together; anything
reasoning in elements must use size and at:.

The byte payload is native-endian, exactly like the snapshot format and actor
message payloads, both of which already round-trip byte-shaped objects verbatim
with no work here."

FloatArray := ArrayedCollection [

	<shape: BytesShape>

	"--- instance creation ---"

	class new: anInteger [
		"anInteger ELEMENTS, hence 8 times as many bytes."
		^self basicNew: anInteger * 8
	]


	class withAll: aCollection [
		| result index |
		result := self new: aCollection size.
		index := 1.
		aCollection do: [:each | result at: index put: each. index := index + 1].
		^result
	]


	"--- accessing ---"

	size [
		"Element count, not byte count. See the class comment."
		^self basicSize bitShift: -3
	]


	at: index [
		<primitive: FloatArrayAtPrimitive>
		^self indexError: index
	]


	at: index put: aNumber [
		"The primitive stores a Float or a SmallInteger directly. Any OTHER
		 Number (a Fraction, a LargeInteger, a ScaledDecimal) is converted here
		 rather than in C, because the conversion is asFloat, a Smalltalk send
		 that a primitive must not make. `at: i put: 1/8` therefore works and
		 stores the nearest double, which is what storing a rational in a float
		 array has to mean."
		<primitive: FloatArrayAtPutPrimitive>
		(aNumber isKindOf: Number) ifFalse: [
			^InvalidArgumentError signal: 'FloatArray holds Numbers, got ', aNumber printString].
		aNumber isFloat ifFalse: [^self at: index put: aNumber asFloat].
		^self indexError: index
	]


	indexError: index [
		"Shared failure tail: the primitive rejects a bad index and an
		 unstorable value alike, so distinguish them here rather than in C."
		^(OutOfRangeError value: index between: 1 and: self size) signal
	]


	"--- bulk arithmetic ---
	 Each of these allocates the destination HERE and hands it to a C kernel,
	 so the kernel is a pure loop over raw buffers that cannot allocate and
	 therefore cannot trigger a collection mid-way."

	+ aFloatArray [
		^self withOperand: aFloatArray op: 1
	]


	- aFloatArray [
		^self withOperand: aFloatArray op: 2
	]


	* aFloatArray [
		"ELEMENTWISE product, not a dot product or a matrix product. Use dot:
		 for the inner product."
		^self withOperand: aFloatArray op: 3
	]


	/ aFloatArray [
		^self withOperand: aFloatArray op: 4
	]


	withOperand: operand op: opcode [
		"A FloatArray operand is elementwise and must match in size; any Number
		 operand is applied as a scalar to every element."
		| result |
		(operand isKindOf: FloatArray)
			ifTrue: [
				operand size = self size ifFalse: [
					^InvalidArgumentError signal: 'FloatArray sizes differ'].
				result := FloatArray new: self size.
				^self primBinary: operand into: result op: opcode]
			ifFalse: [
				(operand isKindOf: Number) ifFalse: [
					^InvalidArgumentError signal: 'expected a FloatArray or a Number'].
				result := FloatArray new: self size.
				^self primScalar: operand into: result op: opcode]
	]


	primBinary: other into: result op: opcode [
		<primitive: FloatArrayBinaryPrimitive>
		^self error: 'FloatArray elementwise operation failed'
	]


	primScalar: scalar into: result op: opcode [
		<primitive: FloatArrayScalarPrimitive>
		^self error: 'FloatArray scalar operation failed'
	]


	dot: aFloatArray [
		"Inner product. Answers a Float."
		<primitive: FloatArrayDotPrimitive>
		(aFloatArray isKindOf: FloatArray) ifFalse: [
			^InvalidArgumentError signal: 'dot: expects a FloatArray'].
		^InvalidArgumentError signal: 'FloatArray sizes differ'
	]


	atAllPut: aNumber [
		<primitive: FloatArrayFillPrimitive>
		(aNumber isKindOf: Number) ifFalse: [
			^InvalidArgumentError signal: 'FloatArray holds Numbers, got ', aNumber printString].
		aNumber isFloat ifFalse: [^self atAllPut: aNumber asFloat].
		^self error: 'FloatArray fill failed'
	]


	"--- reductions ---"

	sum [
		"Overrides Collection>>sum, which walks with a block per element."
		^self primReduce: 1
	]


	max [
		self isEmpty ifTrue: [^EmptyCollectionError signal].
		^self primReduce: 2
	]


	min [
		self isEmpty ifTrue: [^EmptyCollectionError signal].
		^self primReduce: 3
	]


	primReduce: opcode [
		<primitive: FloatArrayReducePrimitive>
		^self error: 'FloatArray reduction failed'
	]

	"Printing needs NO override: Collection>>printOn: walks with do:, which
	 SequenceableCollection defines over `size` and `at:`, and both are element
	 based here. It is Object>>basicPrintOn: that would have printed the raw
	 BYTES, and Collection shadows it."

]
