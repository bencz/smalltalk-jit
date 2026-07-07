OrderedCollection := SequenceableCollection [

	| contents firstIndex lastIndex |


	"instance creation"

	class new [
		^self new: 16
	]


	class new: anInteger [
		^super new initialize: anInteger
	]


	class with: anObject [
		^self new add: anObject; yourself
	]


	class with: anObject with: anObject2 [
		^self new add: anObject; add: anObject2; yourself
	]


	class with: anObject with: anObject2 with: anObject3 [
		^self new add: anObject; add: anObject2; add: anObject3; yourself
	]


	class withAll: aCollection [
		^self new addAll: aCollection; yourself
	]


	"initialization"

	initialize: anInteger [
		contents := Array new: anInteger.
		firstIndex := anInteger // 2 max: 1.
		lastIndex := firstIndex - 1.
	]


	"accessing"

	size [
		^lastIndex - firstIndex + 1
	]


	at: anInteger [
		| index |

		index := anInteger + firstIndex - 1.
		(index >= firstIndex and: [index <= lastIndex])
			ifTrue: [^contents at: index]
			ifFalse: [(OutOfRangeError value: anInteger between: 1 and: self size) signal].
	]


	at: anInteger put: anObject [
		| index |

		index := anInteger + firstIndex - 1.
		(index >= firstIndex and: [index <= lastIndex])
			ifTrue: [^contents at: index put: anObject]
			ifFalse: [(OutOfRangeError value: anInteger between: 1 and: self size) signal].
	]


	after: anObject [
		| index |

		index := self find: anObject.
		index = lastIndex ifTrue: [^self errorLastObject].
		^contents at: index + 1
	]


	before: anObject [
		| index |

		index := self find: anObject.
		index = firstIndex ifTrue: [^self errorFirstObject].
		^contents at: index - 1
	]


	first [
		self emptyCheck.
		^contents at: firstIndex
	]


	last [
		self emptyCheck.
		^contents at: lastIndex
	]


	"adding"

	add: anObject [
		^self addLast: anObject
	]


	addFirst: anObject [
		firstIndex = 1 ifTrue: [self growBy: 8 shiftBy: 8].
		firstIndex := firstIndex - 1.
		^contents at: firstIndex put: anObject
	]


	addLast: anObject [
		lastIndex = contents size ifTrue: [self makeRoomAtLast].
		lastIndex := lastIndex + 1.
		^contents at: lastIndex put: anObject
	]


	makeRoomAtLast [
		"contents is full at the back. If removeFirst has left dead space at the
		 front, slide the live elements down to reuse it (this is what keeps a
		 queue — addLast + removeFirst — from growing its backing array without
		 bound); only grow when there is genuinely no room."
		firstIndex > 1
			ifTrue: [
				| n |
				n := lastIndex - firstIndex + 1.
				contents replaceFrom: 1 to: n with: contents startingAt: firstIndex.
				n + 1 to: contents size do: [:i | contents at: i put: nil].
				firstIndex := 1.
				lastIndex := n]
			ifFalse: [self growBy: 8 shiftBy: 0]
	]


	add: anObject before: oldObject [
		^self add: anObject beforeIndex: (self find: oldObject)
	]


	add: anObject after: oldObject [
		^self add: anObject afterIndex: (self find: oldObject)
	]


	add: anObject beforeIndex: anInteger [
		self insert: anObject beforeIndex: anInteger.
		^anObject
	]


	add: anObject afterIndex: anInteger [
		self insert: anObject beforeIndex: anInteger + 1.
		^anObject
	]


	"removing"

	remove: anObject ifAbsent: aBlock [
		| index |

		index := self
			indexOf: anObject
			startingAt: 1
			ifAbsent: [^aBlock value].
		^self removeIndex: index
	]


	removeFirst [
		| object |

		self emptyCheck.
		object := contents at: firstIndex.
		contents at: firstIndex put: nil.
		firstIndex := firstIndex + 1.
		"emptied: rewind indices so the backing array is reused from the start
		 instead of the window sliding off the end and forcing unbounded growth."
		firstIndex > lastIndex ifTrue: [firstIndex := 1. lastIndex := 0].
		^object
	]


	removeLast [
		| object |

		self emptyCheck.
		object := contents at: lastIndex.
		contents at: lastIndex put: nil.
		lastIndex := lastIndex - 1.
		lastIndex < firstIndex ifTrue: [firstIndex := 1. lastIndex := 0].
		^object
	]


	"enumerating"

	do: aBlock [
		firstIndex to: lastIndex do: [ :i | aBlock value: (contents at: i)].
	]


	collect: aBlock [
		| newCollection |

		newCollection := self class new: self size.
		1 to: self size do: [ :index | newCollection add: (aBlock value: (self at: index))].
		^newCollection
	]


	"private"

	growBy: growSize shiftBy: shift [
		| newContents |

		newContents := contents species new: contents size + growSize.
		newContents
			replaceFrom: firstIndex + shift to: lastIndex + shift
			with: contents
			startingAt: firstIndex.
		contents := newContents.
		firstIndex := firstIndex + shift.
		lastIndex := lastIndex + shift.
	]


	find: anObject [
		^self indexOf: anObject ifAbsent: [(NotFoundError value: anObject) signal]
	]


	insert: anObject beforeIndex: anInteger [
		"Opens a hole by shifting elements toward the back, so we need
		 room at the back: grow when lastIndex has reached the end of
		 contents (not merely when the collection is completely full,
		 since free slots may sit at the front)."
		lastIndex = contents size ifTrue: [self growBy: 8 shiftBy: 0].
		lastIndex := lastIndex + 1.
		lastIndex to: anInteger + firstIndex by: -1 do: [ :i |
			contents at: i put: (contents at: i - 1)].
		contents at: anInteger + firstIndex - 1 put: anObject.
	]


	removeIndex: anInteger [
		| object |

		object := self at: anInteger.
		contents replaceFrom: anInteger + firstIndex - 1
			to: lastIndex - 1
			with: contents
			startingAt: anInteger + firstIndex.
		contents at: lastIndex put: nil.
		lastIndex := lastIndex - 1.
		^object
	]


	"copying"

	, aCollection [
		| new |

		new := OrderedCollection new.
		new addAll: self.
		new addAll: aCollection.
		^new
	]


	reverse [
		| new |

		new := OrderedCollection new.
		self reverseDo: [ :v | new add: v].
		^new
	]


	copyFrom: start to: stop [
		| new |

		new := OrderedCollection new.
		start to: stop do: [ :i | new add: (self at: i)].
		^new
	]


	"printing"

	examineOn: aStream [
		super examineOn: aStream.
		self do: [ :v |
			aStream nextPutAll: ' - '.
			v == self
				ifTrue: [aStream nextPutAll: 'self (recursion)']
				ifFalse: [v printOn: aStream].
			aStream lf].
	]

]
