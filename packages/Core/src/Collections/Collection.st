Collection := Iterator [

	"streaming"

	class streamContents: aBlock [
		"Create a write stream over a fresh instance of the receiver species,
		 run aBlock against it and answer the written contents. Intended for
		 arrayed species (String, Array): the underlying CollectionStream grows
		 its buffer via copyResized:."
		| stream |

		stream := CollectionStream with: (self new: 16).
		aBlock value: stream.
		^stream contents
	]


	"accessing"

	size [
		| tally |

		tally := 0.
		self do: [ :each | tally := tally + 1].
		^tally
	]


	"testing"

	includes: anObject [
		self do: [ :v | v = anObject ifTrue: [^true]].
		^false
	]


	includesAnyOf: aCollection [
		^self anySatisfy: [ :v | aCollection includes: v]
	]


	isEmpty [
		^self size = 0
	]


	occurrencesOf: anObject [
		| tally |

		tally := 0.
		self do: [ :v | v = anObject ifTrue: [tally := tally + 1]].
		^tally
	]


	"adding"

	add: anObject [
		SubClassResponsibility signal.
	]


	addAll: aCollection [
		aCollection do: [ :v | self add: v].
		^aCollection
	]


	"removing"

	remove: anObject [
		^self remove: anObject ifAbsent: [(NotFoundError value: anObject) signal]
	]


	remove: anObject ifAbsent: aBlock [
		SubClassResponsibility signal.
	]


	removeAll: aCollection [
		aCollection do: [ :v | self remove: v].
		^aCollection
	]


	"private"

	emptyCheck [
		self isEmpty ifTrue: [OutOfRangeError signal].
	]


	"printing"

	printOn: aStream [
		| items |

		super printOn: aStream.

		items := 0.
		aStream nextPutAll: ' ('.
		self do: [ :v |
			(items := items + 1) = 201 ifTrue: [
				aStream nextPutAll: '...)'.
				^self].
			v == self
				ifTrue: [aStream nextPutAll: 'self(recursion)']
				ifFalse: [v printOn: aStream].
			aStream space].
		aStream nextPut: $).
	]


	"enumerating"

	select: aBlock [
		| result |

		result := OrderedCollection new.
		self do: [ :v | (aBlock value: v) ifTrue: [result add: v]].
		^result
	]


	reject: aBlock [
		^self select: [ :v | (aBlock value: v) not]
	]


	collect: aBlock [
		| result |

		result := OrderedCollection new.
		self do: [ :v | result add: (aBlock value: v)].
		^result
	]


	detect: aBlock ifNone: aNoneBlock [
		self do: [ :v | (aBlock value: v) ifTrue: [^v]].
		^aNoneBlock value
	]


	do: aBlock separatedBy: separatorBlock [
		| first |

		first := true.
		self do: [ :v |
			first ifTrue: [first := false] ifFalse: [separatorBlock value].
			aBlock value: v].
	]


	count: aBlock [
		| tally |

		tally := 0.
		self do: [ :v | (aBlock value: v) ifTrue: [tally := tally + 1]].
		^tally
	]


	"reducing"

	sum [
		| total first |

		total := 0.
		first := true.
		self do: [ :v |
			first
				ifTrue: [total := v. first := false]
				ifFalse: [total := total + v]].
		^total
	]


	max [
		| result first |

		first := true.
		self do: [ :v |
			first
				ifTrue: [result := v. first := false]
				ifFalse: [v > result ifTrue: [result := v]]].
		first ifTrue: [^self error: 'max of empty collection'].
		^result
	]


	min [
		| result first |

		first := true.
		self do: [ :v |
			first
				ifTrue: [result := v. first := false]
				ifFalse: [v < result ifTrue: [result := v]]].
		first ifTrue: [^self error: 'min of empty collection'].
		^result
	]


	average [
		self isEmpty ifTrue: [^self error: 'average of empty collection'].
		^self sum / self size
	]


	"reducing by a block"

	sum: aBlock [
		"Sum of the block values. Answers 0 for an empty collection."
		| total |

		total := 0.
		self do: [ :v | total := total + (aBlock value: v)].
		^total
	]


	max: aBlock [
		| result first |

		first := true.
		self do: [ :v | | mapped |
			mapped := aBlock value: v.
			first
				ifTrue: [result := mapped. first := false]
				ifFalse: [mapped > result ifTrue: [result := mapped]]].
		first ifTrue: [^self error: 'max: of empty collection'].
		^result
	]


	min: aBlock [
		| result first |

		first := true.
		self do: [ :v | | mapped |
			mapped := aBlock value: v.
			first
				ifTrue: [result := mapped. first := false]
				ifFalse: [mapped < result ifTrue: [result := mapped]]].
		first ifTrue: [^self error: 'min: of empty collection'].
		^result
	]


	"enumerating - extensions"

	flatCollect: aBlock [
		"Collect the block value for each element (each expected to be a
		 collection) and concatenate them into one OrderedCollection."
		| result |

		result := OrderedCollection new.
		self do: [ :v | result addAll: (aBlock value: v)].
		^result
	]


	gather: aBlock [
		^self flatCollect: aBlock
	]


	allSatisfy: aBlock [
		self do: [ :v | (aBlock value: v) ifFalse: [^false]].
		^true
	]


	noneSatisfy: aBlock [
		self do: [ :v | (aBlock value: v) ifTrue: [^false]].
		^true
	]


	detect: aBlock ifFound: foundBlock ifNone: noneBlock [
		self do: [ :v | (aBlock value: v) ifTrue: [^foundBlock value: v]].
		^noneBlock value
	]


	select: selectBlock thenCollect: collectBlock [
		| result |

		result := OrderedCollection new.
		self do: [ :v |
			(selectBlock value: v) ifTrue: [result add: (collectBlock value: v)]].
		^result
	]


	reject: rejectBlock thenCollect: collectBlock [
		| result |

		result := OrderedCollection new.
		self do: [ :v |
			(rejectBlock value: v) ifFalse: [result add: (collectBlock value: v)]].
		^result
	]


	collect: aBlock as: aClass [
		| result |

		result := OrderedCollection new.
		self do: [ :v | result add: (aBlock value: v)].
		^aClass withAll: result
	]


	groupBy: aBlock [
		"Answer a Dictionary mapping each key produced by aBlock to an
		 OrderedCollection of the elements yielding that key."
		| result |

		result := Dictionary new.
		self do: [ :v |
			(result at: (aBlock value: v) ifAbsentPut: [OrderedCollection new]) add: v].
		^result
	]


	"testing - membership over collections"

	includesAll: aCollection [
		aCollection do: [ :v | (self includes: v) ifFalse: [^false]].
		^true
	]


	includesAny: aCollection [
		aCollection do: [ :v | (self includes: v) ifTrue: [^true]].
		^false
	]


	"set operations - answer a Set"

	union: aCollection [
		| result |

		result := Set new.
		result addAll: self.
		result addAll: aCollection.
		^result
	]


	intersection: aCollection [
		| result other |

		other := aCollection asSet.
		result := Set new.
		self do: [ :v | (other includes: v) ifTrue: [result add: v]].
		^result
	]


	difference: aCollection [
		| result other |

		other := aCollection asSet.
		result := Set new.
		self do: [ :v | (other includes: v) ifFalse: [result add: v]].
		^result
	]


	asDictionary [
		"Build a Dictionary from a collection of Associations."
		| result |

		result := Dictionary new.
		self do: [ :assoc | result add: assoc].
		^result
	]


	"testing"

	notEmpty [
		^self isEmpty not
	]


	"converting"

	asArray [
		| array index |

		array := Array new: self size.
		index := 0.
		self do: [ :v | array at: (index := index + 1) put: v].
		^array
	]


	asOrderedCollection [
		| result |

		result := OrderedCollection new.
		self do: [ :v | result add: v].
		^result
	]


	asSet [
		| result |

		result := Set new.
		self do: [ :v | result add: v].
		^result
	]


	asBag [
		| result |

		result := Bag new.
		self do: [ :v | result add: v].
		^result
	]


	asSortedCollection [
		^SortedCollection withAll: self
	]


	asSortedCollection: aBlock [
		| result |

		result := SortedCollection sortBlock: aBlock.
		result addAll: self.
		^result
	]

]
