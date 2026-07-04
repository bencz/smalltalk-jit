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
		| index limit |

		limit := self size.
		index := 1.
		[index <= limit and: [self sortBlock value: (self at: index) value: anObject]]
			whileTrue: [index := index + 1].
		self add: anObject beforeIndex: index.
		^anObject
	]


	addFirst: anObject [
		^self add: anObject
	]


	addLast: anObject [
		^self add: anObject
	]
]
