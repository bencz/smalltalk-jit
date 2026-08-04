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

"A loop whose condition answers a non-Boolean must RAISE, and these are written
 so that a regression FAILS rather than HANGS.

 The obvious form is `[[] whileTrue]`, and it was here: with the guard it raises
 on the first evaluation, and without it nil is not false, so it falls into the
 body and the loop never comes back. That is what it did -- this file hung for
 two minutes on every suite run and reported nothing about anything below it.

 So each condition answers its non-Boolean a few times and then a real Boolean.
 A guarded loop still raises on the FIRST evaluation, which is what is being
 checked; an unguarded one merely terminates, and the missing exception is an
 ordinary red check that costs milliseconds."
[
	| n |
	BlkRun expect: 'whileTrue on nil result raises' raises: Error in: [
		n := 0. [n := n + 1. n < 5 ifTrue: [nil] ifFalse: [false]] whileTrue].
	BlkRun expect: 'whileTrue on integer raises' raises: Error in: [
		n := 0. [n := n + 1. n < 5 ifTrue: [1] ifFalse: [false]] whileTrue].
	BlkRun expect: 'whileTrue on class raises' raises: Error in: [
		n := 0. [n := n + 1. n < 5 ifTrue: [Object] ifFalse: [false]] whileTrue].
	BlkRun expect: 'whileTrue: on nil result raises' raises: Error in: [
		n := 0. [n := n + 1. n < 5 ifTrue: [nil] ifFalse: [false]] whileTrue: []].
	BlkRun expect: 'whileTrue: on integer raises' raises: Error in: [
		n := 0. [n := n + 1. n < 5 ifTrue: [1] ifFalse: [false]] whileTrue: []].
	BlkRun expect: 'whileTrue: on class raises' raises: Error in: [
		n := 0. [n := n + 1. n < 5 ifTrue: [Object] ifFalse: [false]] whileTrue: []].
]


[ ^BlkRun report ]
