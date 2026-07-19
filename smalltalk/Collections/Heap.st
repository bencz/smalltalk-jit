"A binary heap kept over an internal OrderedCollection. The sort block is a
 two-argument predicate answering true when its first argument should be at or
 nearer the top than its second; the default [:a :b | a <= b] makes a min-heap
 (first answers the smallest element). #first / #removeFirst read and remove the
 top in O(log n); #add: is O(log n). Iteration order (#do:, #asArray, #includes:)
 follows the internal array and is NOT sorted."

Heap := Collection [

	| contents sortBlock |


	"instance creation"

	class new [
		^self basicNew initializeHeap
	]


	class sortBlock: aBlock [
		^self basicNew initializeHeap setSortBlock: aBlock
	]


	class withAll: aCollection [
		^self new addAll: aCollection; yourself
	]


	"initialization"

	initializeHeap [
		contents := OrderedCollection new.
		^self
	]


	setSortBlock: aBlock [
		sortBlock := aBlock.
		^self
	]


	sortBlock [
		sortBlock isNil ifTrue: [sortBlock := [ :a :b | a <= b]].
		^sortBlock
	]


	"accessing"

	size [
		^contents size
	]


	first [
		self emptyCheck.
		^contents first
	]


	"testing"

	isEmpty [
		^contents isEmpty
	]


	notEmpty [
		^contents notEmpty
	]


	includes: anObject [
		^contents includes: anObject
	]


	"adding"

	add: anObject [
		contents addLast: anObject.
		self siftUp: contents size.
		^anObject
	]


	"removing"

	removeFirst [
		| top last |

		self emptyCheck.
		top := contents first.
		last := contents removeLast.
		contents isEmpty ifFalse: [
			contents at: 1 put: last.
			self siftDown: 1].
		^top
	]


	"enumerating"

	do: aBlock [
		contents do: aBlock.
	]


	"converting"

	asArray [
		^contents asArray
	]


	"private - heap maintenance"

	siftUp: anIndex [
		| i parent tmp block |

		block := self sortBlock.
		i := anIndex.
		[i > 1 and: [(block value: (contents at: i // 2) value: (contents at: i)) not]]
			whileTrue: [
				parent := i // 2.
				tmp := contents at: parent.
				contents at: parent put: (contents at: i).
				contents at: i put: tmp.
				i := parent].
	]


	siftDown: anIndex [
		| i size child tmp block |

		block := self sortBlock.
		i := anIndex.
		size := contents size.
		[true] whileTrue: [
			child := i * 2.
			child > size ifTrue: [^self].
			(child < size and: [(block value: (contents at: child) value: (contents at: child + 1)) not])
				ifTrue: [child := child + 1].
			(block value: (contents at: i) value: (contents at: child))
				ifTrue: [^self]
				ifFalse: [
					tmp := contents at: i.
					contents at: i put: (contents at: child).
					contents at: child put: tmp.
					i := child]].
	]

]
