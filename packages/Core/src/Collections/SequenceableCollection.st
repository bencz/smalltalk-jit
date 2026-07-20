SequenceableCollection := Collection [

	"comparing"

	hash [
		| hash |

		hash := self class hash.
		self do: [ :v | hash := hash bitXor: v hash].
		^hash
	]


	= aCollection [
		| size |

		self class == aCollection class ifFalse: [^false].
		(size := self size) = aCollection size ifFalse: [^false].

		1 to: size do: [ :i |
			(self at: i) = (aCollection at: i) ifFalse: [^false]].
		^true
	]


	startsWith: aSequenceableCollection [
		aSequenceableCollection size > self size ifTrue: [^false].
		aSequenceableCollection keysAndValuesDo: [ :i :c |
			(self at: i) = c ifFalse: [^false]].
		^true
	]


	"enumerating"

	do: aBlock [
		1 to: self size do: [ :i | aBlock value: (self at: i)].
	]


	reverseDo: aBlock [
		self size to: 1 by: -1 do: [ :i | aBlock value: (self at: i)].
	]


	keysAndValuesDo: aBlock [
		1 to: self size do: [ :i | aBlock value: i value: (self at: i)].
	]


	includes: anObject [
		1 to: self size do: [ :index | anObject = (self at: index) ifTrue: [^true]].
		^false
	]


	collect: aBlock [
		| newCollection |

		newCollection := self species new: self size.
		1 to: self size do: [ :index | newCollection at: index put: (aBlock value: (self at: index))].
		^newCollection
	]


	"accessing"

	size [
		SubClassResponsibility signal.
	]


	first [
		^self at: 1
	]


	last [
		^self at: self size
	]


	indexOf: anObject [
		^self indexOf: anObject ifAbsent: [0]
	]


	indexOf: anObject ifAbsent: aBlock [
		^self indexOf: anObject startingAt: 1 ifAbsent: aBlock
	]


	indexOf: anObject startingAt: anInteger ifAbsent: aBlock [
		anInteger to: self size do: [ :i |
			(self at: i) = anObject ifTrue: [^i]].
		^aBlock value
	]


	lastIndexOf: anObject [
		self size to: 1 by: -1 do: [ :i |
			(self at: i) = anObject ifTrue: [^i]].
		^0
	]


	indexOfSubCollection: aCollection startingAt: anInteger [
		^self
			indexOfSubCollection: aCollection
			startingAt: anInteger
			ifAbsent: [0]
	]


	indexOfSubCollection: aCollection startingAt: anInteger ifAbsent: aBlock [
		| result first |

		aCollection size = 0 ifTrue: [^aBlock value].
		first := aCollection at: 1.

		anInteger to: self size - aCollection size + 1 do: [ :i |
			result := true.
			(self at: i) = first ifTrue: [
				1 to: aCollection size do: [ :j |
					(self at: i + j - 1) = (aCollection at: j) ifFalse: [result := false]].
			result ifTrue: [^i]]].

		^aBlock value
	]


	replaceFrom: start to: stop with: replacement [
		self replaceFrom: start to: stop with: replacement startingAt: 1.
	]


	replaceFrom: start to: stop with: replacement startingAt: replacementStart [
		"Bulk memmove for byte collections (String/Symbol/ByteArray); the loop is
		 the fallback for pointer Arrays (its at:put: runs the GC write barrier)."
		<primitive: ReplaceBytesPrimitive>
		start to: stop do: [ :i |
			self at: i put: (replacement at: i - start + replacementStart) ].
	]


	splitBy: anObject [
		"C fast path for a plain String split on a Character (scan + build the
		 OrderedCollection in one primitive). Array / Symbol / non-char delimiters
		 fall through to the generic loop below."
		<primitive: StringSplitByPrimitive>
		| collection last |

		collection := OrderedCollection new.
		last := 1.

		1 to: self size do: [ :i |
			(self at: i) = anObject ifTrue: [
				last = i ifFalse: [collection addLast: (self copyFrom: last to: i - 1)].
				last := i + 1.
			].
		].
		last <= self size ifTrue: [collection addLast: (self copyFrom: last to: self size)].
		^collection
	]


	splitByAll: aCollection[
		| collection last |

		aCollection size = 1 ifTrue: [^self splitBy: aCollection first].
		collection := OrderedCollection new.
		last := 1.

		1 to: self size do: [ :i |
			(aCollection includes: (self at: i)) ifTrue: [
				last = i ifFalse: [collection addLast: (self copyFrom: last to: i - 1)].
				last := i + 1.
			].
		].
		last <= self size ifTrue: [collection addLast: (self copyFrom: last to: self size)].
		^collection
	]


	"removing"

	remove: anObject ifAbsent: aBlock [
		ShouldNotImplement signal.
	]


	"copying"

	, aCollection [
		^self copyReplaceFrom: self size + 1
			to: self size
			with: aCollection
	]


	copyFrom: start [
		^self copyFrom: start to: self size
	]


	copyFrom: start to: stop [
		"C fast path for plain Strings (allocate + memcpy). Symbol/Array/other
		 species fall through to the generic species/replaceFrom: path below."
		<primitive: StringCopyFromToPrimitive>
		| newSize |

		newSize := stop - start + 1.
		^(self species new: newSize)
			replaceFrom: 1
			to: newSize
			with: self
			startingAt: start
	]


	copyResized: anInteger [
		| newCollection |

		newCollection := self species new: anInteger.
		newCollection
			replaceFrom: 1
			to: (self size min: anInteger)
			with: self
			startingAt: 1.
		^newCollection
	]


	copyReplaceFrom: start to: stop with: replacementCollection [
		| newCollection newSize endReplacement |

		newSize := self size - (stop - start + 1) + replacementCollection size.
		endReplacement := start - 1 + replacementCollection size.
		newCollection := self species new: newSize.

		newCollection
			replaceFrom: 1
			to: start - 1
			with: self
			startingAt: 1.
		newCollection
			replaceFrom: start
			to: endReplacement
			with: replacementCollection
			startingAt: 1.
		newCollection
			replaceFrom: endReplacement + 1
			to: newSize
			with: self
			startingAt: stop + 1.
		^newCollection
	]


	copyWith: anObject [
		| newCollection newSize |

		newSize := self size + 1.
		newCollection := self copyResized: newSize.
		newCollection at: newSize put: anObject.
		^newCollection
	]


	copyUpTo: anObject [
		^self
			copyFrom: 1
			to: (self indexOf: anObject ifAbsent: [self size + 1]) - 1
	]


	copyAfter: anObject [
		^self copyFrom: (self indexOf: anObject ifAbsent: [self size]) + 1
	]


	"converting"

	asArray [
		^(Array new: self size) replaceFrom: 1 to: self size with: self
	]


	reverse [
		| size result |

		size := self size.
		result := self species new: size.
		1 to: size do: [ :i | result at: i put: (self at: size - i + 1)].
		^result
	]


	reversed [
		^self reverse
	]


	with: aCollection collect: aBlock [
		| size result |

		size := self size.
		result := self species new: size.
		1 to: size do: [ :i |
			result at: i put: (aBlock value: (self at: i) value: (aCollection at: i))].
		^result
	]


	with: aCollection do: aBlock [
		1 to: self size do: [ :i |
			aBlock value: (self at: i) value: (aCollection at: i)].
	]


	"enumerating with index (Pharo order: element then index)"

	doWithIndex: aBlock [
		1 to: self size do: [ :i | aBlock value: (self at: i) value: i].
	]


	withIndexDo: aBlock [
		self doWithIndex: aBlock.
	]


	"accessing prefixes and suffixes"

	first: n [
		^self copyFrom: 1 to: n
	]


	last: n [
		^self copyFrom: self size - n + 1 to: self size
	]


	allButFirst [
		^self allButFirst: 1
	]


	allButLast [
		^self allButLast: 1
	]


	allButFirst: n [
		^self copyFrom: n + 1 to: self size
	]


	allButLast: n [
		^self copyFrom: 1 to: self size - n
	]


	"testing"

	endsWith: aCollection [
		| offset |

		aCollection isEmpty ifTrue: [^false].
		aCollection size > self size ifTrue: [^false].
		offset := self size - aCollection size.
		1 to: aCollection size do: [ :i |
			(self at: offset + i) = (aCollection at: i) ifFalse: [^false]].
		^true
	]


	"bulk mutation"

	atAll: indexCollection put: anObject [
		indexCollection do: [ :i | self at: i put: anObject].
		^anObject
	]


	atAllPut: anObject [
		1 to: self size do: [ :i | self at: i put: anObject].
		^anObject
	]


	"copying"

	copyWithout: oldObject [
		| kept |

		kept := OrderedCollection new.
		self do: [ :v | v = oldObject ifFalse: [kept add: v]].
		^self species withAll: kept
	]


	copyReplaceAll: oldSubCollection with: newSubCollection [
		"Replace every non-overlapping occurrence of oldSubCollection with
		 newSubCollection, scanning left to right with the existing
		 indexOfSubCollection:startingAt:."
		| result index found |

		oldSubCollection isEmpty ifTrue: [^self copyFrom: 1 to: self size].
		result := OrderedCollection new.
		index := 1.
		[index <= self size] whileTrue: [
			found := self indexOfSubCollection: oldSubCollection startingAt: index.
			found = 0
				ifTrue: [
					index to: self size do: [ :i | result add: (self at: i)].
					index := self size + 1]
				ifFalse: [
					index to: found - 1 do: [ :i | result add: (self at: i)].
					newSubCollection do: [ :e | result add: e].
					index := found + oldSubCollection size]].
		^self species withAll: result
	]


	"sorting"

	isSorted [
		^self isSorted: [ :a :b | a <= b]
	]


	isSorted: aSortBlock [
		| prev first |

		first := true.
		self do: [ :each |
			first
				ifTrue: [first := false]
				ifFalse: [(aSortBlock value: prev value: each) ifFalse: [^false]].
			prev := each].
		^true
	]


	sort [
		^self sort: [ :a :b | a <= b]
	]


	sort: aSortBlock [
		"Stable in-place merge sort. Requires at:put: on the receiver; species
		 without it (e.g. Interval) raise their natural error."
		| work |

		self size <= 1 ifTrue: [^self].
		work := Array new: self size.
		self mergeSortFrom: 1 to: self size by: aSortBlock into: work.
		^self
	]


	mergeSortFrom: low to: high by: aSortBlock into: work [
		| mid i j k |

		low >= high ifTrue: [^self].
		mid := (low + high) // 2.
		self mergeSortFrom: low to: mid by: aSortBlock into: work.
		self mergeSortFrom: mid + 1 to: high by: aSortBlock into: work.
		low to: high do: [ :x | work at: x put: (self at: x)].
		i := low.
		j := mid + 1.
		k := low.
		[k <= high] whileTrue: [
			(j > high or: [i <= mid and: [aSortBlock value: (work at: i) value: (work at: j)]])
				ifTrue: [self at: k put: (work at: i). i := i + 1]
				ifFalse: [self at: k put: (work at: j). j := j + 1].
			k := k + 1].
	]


	sorted [
		^self sorted: [ :a :b | a <= b]
	]


	sorted: aSortBlock [
		"Answer a sorted copy as an Array; the receiver is left unchanged."
		| array |

		array := self asArray.
		array sort: aSortBlock.
		^array
	]


	"shuffling and random access"

	shuffle [
		^self shuffleWith: Random default
	]


	shuffleWith: aRandom [
		"In-place Fisher-Yates."
		| j tmp |

		self size to: 2 by: -1 do: [ :i |
			j := aRandom nextInteger: i.
			tmp := self at: i.
			self at: i put: (self at: j).
			self at: j put: tmp].
		^self
	]


	shuffled [
		^self asArray shuffleWith: Random default
	]


	atRandom [
		^self atRandom: Random default
	]


	atRandom: aRandom [
		self emptyCheck.
		^self at: (aRandom nextInteger: self size)
	]


	pairsDo: aBlock [
		"Evaluate aBlock with each consecutive non-overlapping pair of elements."
		1 to: self size - 1 by: 2 do: [ :i |
			aBlock value: (self at: i) value: (self at: i + 1)].
	]


	"streaming"

	readStream [
		^CollectionStream on: self
	]


	writeStream [
		"A write stream over a fresh growable buffer of the same species."
		^CollectionStream with: (self species new: 16)
	]

]
