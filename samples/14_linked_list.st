"============================================================
  14 - Singly Linked List
  ------------------------------------------------------------
  A classic pointer-based data structure built from scratch:
  nodes that each hold a value and a reference to the next node.
  ============================================================"

LinkedNode := Object [
	| value next |
	class value: aValue next: aNode [ ^self new setValue: aValue next: aNode ]
	setValue: aValue next: aNode [ value := aValue. next := aNode ]
	value [ ^value ]
	next  [ ^next ]
	next: aNode [ next := aNode ]
]

"Subclassing Collection means we get the whole enumeration
 protocol (collect:, select:, inject:into:, includes:, detect: ...)
 for free, as long as we implement do:."
LinkedList := Collection [
	| head tail count |

	class new [ ^super new init ]
	init [ head := nil. tail := nil. count := 0 ]

	size [ ^count ]
	isEmpty [ ^count = 0 ]

	add: aValue [ ^self addLast: aValue ]

	addLast: aValue [
		| node |
		node := LinkedNode value: aValue next: nil.
		head isNil
			ifTrue:  [head := node]
			ifFalse: [tail next: node].
		tail := node.
		count := count + 1.
		^aValue
	]

	"The enumeration protocol only needs do: - everything else
	 (collect:, select:, inject:into:, printOn: ...) is inherited."
	do: aBlock [
		| node |
		node := head.
		[node isNil] whileFalse: [
			aBlock value: node value.
			node := node next].
	]

	asArray [
		| array index |
		array := Array new: count.
		index := 0.
		self do: [:v | array at: (index := index + 1) put: v].
		^array
	]

	printOn: aStream [
		aStream nextPutAll: 'LinkedList('.
		self do: [:v | v printOn: aStream. aStream space].
		aStream nextPut: $).
	]
]

[
	| list |
	list := LinkedList new.
	list addLast: 10; addLast: 20; addLast: 30; addLast: 40.

	Transcript nextPutAll: 'list     = '. list printNl.
	Transcript nextPutAll: 'size     = '. list size printNl.
	Transcript nextPutAll: 'asArray  = '. list asArray printNl.
	Transcript nextPutAll: 'doubled  = '. (list collect: [:v | v * 2]) printNl.
	Transcript nextPutAll: 'sum      = '. (list inject: 0 into: [:a :b | a + b]) printNl.
	Transcript nextPutAll: 'has 30?  = '. (list includes: 30) printNl.
]
