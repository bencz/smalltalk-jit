"A SortedCollection keeps its elements ordered according to a
 sort block, a two-argument block answering true when its first
 argument should come before-or-at the same place as its second.
 The default sort block orders elements ascending using <=."

SortedCollection := OrderedCollection [

	| sortBlock |


	"instance creation"

	class sortBlock: aBlock [
		| collection |

		collection := self new.
		collection setSortBlock: aBlock.
		^collection
	]


	"initialization"

	setSortBlock: aBlock [
		sortBlock := aBlock.
	]


	sortBlock [
		sortBlock isNil ifTrue: [sortBlock := [ :a :b | a <= b]].
		^sortBlock
	]


	"adding - insertion keeps the collection ordered"

	add: anObject [
		"Binary search for the insertion point. To stay stable, anObject is
		 inserted AFTER every element the sort block already ranks before-or-
		 equal to it: the search keeps moving low past any index where
		 sortBlock(element, anObject) holds."
		| low high mid block |

		block := self sortBlock.
		low := 1.
		high := self size.
		[low <= high] whileTrue: [
			mid := (low + high) // 2.
			(block value: (self at: mid) value: anObject)
				ifTrue: [low := mid + 1]
				ifFalse: [high := mid - 1]].
		self add: anObject beforeIndex: low.
		^anObject
	]


	addFirst: anObject [
		^self add: anObject
	]


	addLast: anObject [
		^self add: anObject
	]
]
