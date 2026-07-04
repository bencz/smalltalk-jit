Collection := Iterator [

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
		^result
	]


	min [
		| result first |

		first := true.
		self do: [ :v |
			first
				ifTrue: [result := v. first := false]
				ifFalse: [v < result ifTrue: [result := v]]].
		^result
	]


	average [
		^self sum / self size
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
