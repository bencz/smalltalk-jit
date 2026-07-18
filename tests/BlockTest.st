BlockTest := Object [

	class outerReturn [
		[^1] value.
	]

]

[
	BlkRun := TestRun named: 'Block'.

	BlkRun check: 'empty block value isNil' that: [[] value isNil].
	BlkRun check: 'block value' equals: 1 is: [[1] value].
	BlkRun check: 'outer return value' equals: 1 is: [BlockTest outerReturn].
]

[
	BlkRun expect: 'value with missing arg raises' raises: OutOfRangeError in: [[ :a | ] value].
	BlkRun expect: 'value with extra arg raises' raises: OutOfRangeError in: [[ :a | ] value: 1 value: 2].
]

[
	| block |

	block := [false].
	BlkRun check: 'whileTrue returns receiver' that: [block whileTrue == block].
	BlkRun check: 'whileTrue: returns receiver' that: [(block whileTrue: []) == block].
]

[
	BlkRun expect: 'whileTrue on nil result raises' raises: Error in: [[] whileTrue].
	BlkRun expect: 'whileTrue on integer raises' raises: Error in: [[1] whileTrue].
	BlkRun expect: 'whileTrue on class raises' raises: Error in: [[Object] whileTrue].
	BlkRun expect: 'whileTrue: on nil result raises' raises: Error in: [[] whileTrue: []].
	BlkRun expect: 'whileTrue: on integer raises' raises: Error in: [[1] whileTrue: []].
	BlkRun expect: 'whileTrue: on class raises' raises: Error in: [[Object] whileTrue: []].
]


[ ^BlkRun report ]
