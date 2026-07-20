"LazySequence: a LINQ-to-Objects style pipeline for Smalltalk with deferred
 (lazy) execution. It is the query-comprehension face of the whole iteration
 protocol: because it subclasses Iterator (the root of Collection and Stream),
 and because Iterator answers #asLazy, EVERY collection and every stream can
 start a lazy pipeline for free with `aCollection asLazy` (alias `lazy`).

 DESIGN, push model. A LazySequence wraps a `source` that answers #do:. Each
 intermediate operator (collect:, select:, take:, ...) returns a tiny stage
 object, a subclass of LazySequence, that remembers its `upstream` and its
 parameters but does NO work. Iteration only happens when a terminal (asArray,
 sum, first, do:, ...) finally sends #do:. A stage's #do: drives its upstream:
 `upstream do: [:each | ...transform or filter... aBlock value: ...]`. So the
 elements are PUSHED downstream one at a time and nothing is materialized in
 the middle unless an operator explicitly needs to (see the sort barrier).

 EARLY TERMINATION without a sentinel exception. Bounded operators stop the
 upstream with a non-local return out of their own #do: method. For example
 take:'s #do: does `... seen >= count ifTrue: [^self] ...`; the ^self returns
 from that #do:, which unwinds the upstream #do: loop cleanly, running any
 ensure:/ifCurtailed: handlers on the way out. #first is just
 `self do: [:each | ^each]`. This is the same mechanism exercised by
 tests/OuterReturnTest.st and it is fully supported by the runtime.

 PER-ITERATION STATE. Stateful stages (take:, skip:, takeWhile:, skipWhile:,
 distinct, zip:, withIndexCollect:) MUST create their counters/sets INSIDE
 #do: as temporaries captured by the pushed block, never in an instance
 variable. That is what lets the very same pipeline object be iterated many
 times and give an identical result every time: each iteration gets a fresh
 counter/Set/index and never sees leftovers from a previous run.

 The stage blocks are kept trivial (one #value: per element) so the JIT can
 inline them well."

LazySequence := Iterator [

	| source |


	"--- instance creation ---"

	class on: aSource [
		"aSource can be anything that answers #do:, a collection, a stream, an
		 interval, or another LazySequence."
		^self new setSource: aSource
	]


	setSource: aSource [
		source := aSource.
		^self
	]


	"--- the engine: every terminal and every stage bottoms out here ---"

	do: aBlock [
		source do: aBlock
	]


	"--- intermediate operators (lazy, return a new stage) ---"

	collect: aBlock [
		^LazyCollectStage upstream: self block: aBlock
	]


	select: aBlock [
		^LazySelectStage upstream: self block: aBlock
	]


	where: aBlock [
		"LINQ alias of #select: (filter). LINQ Select is #collect:, so we do NOT
		 alias #select: to a LINQ name to avoid the map/filter ambiguity."
		^self select: aBlock
	]


	reject: aBlock [
		^LazyRejectStage upstream: self block: aBlock
	]


	flatCollect: aBlock [
		"aBlock must answer something that responds to #do:; flattens one level."
		^LazyFlatCollectStage upstream: self block: aBlock
	]


	selectMany: aBlock [
		"LINQ alias of #flatCollect:."
		^self flatCollect: aBlock
	]


	take: anInteger [
		^LazyTakeStage upstream: self count: anInteger
	]


	skip: anInteger [
		^LazySkipStage upstream: self count: anInteger
	]


	takeWhile: aBlock [
		^LazyTakeWhileStage upstream: self block: aBlock
	]


	skipWhile: aBlock [
		^LazySkipWhileStage upstream: self block: aBlock
	]


	distinct [
		^LazyDistinctStage upstream: self
	]


	zip: aCollection [
		"Pairs each upstream element with the positionally matching element of
		 aCollection as (elem1 -> elem2), stopping at the shorter side."
		^LazyZipStage upstream: self other: aCollection
	]


	withIndexCollect: aBlock [
		"aBlock receives (element, index) with a 1-based index."
		^LazyWithIndexCollectStage upstream: self block: aBlock
	]


	"--- barriers (materialize INSIDE #do:, still lazy and re-iterable) ---"

	sorted [
		^self sorted: [ :a :b | a <= b]
	]


	sorted: aBlock [
		"aBlock value: a value: b answers true when a should come before-or-with
		 b (stable). Materializes the upstream and sorts a private copy each time
		 it is iterated, so re-iteration is deterministic."
		^LazySortStage upstream: self sortBlock: aBlock
	]


	orderBy: keyBlock [
		^self sorted: [ :a :b | (keyBlock value: a) <= (keyBlock value: b)]
	]


	orderByDescending: keyBlock [
		^self sorted: [ :a :b | (keyBlock value: a) >= (keyBlock value: b)]
	]


	groupBy: keyBlock [
		"TERMINAL in practice: forces the whole upstream and answers a Dictionary
		 from key to an OrderedCollection of the elements with that key."
		| dict |

		dict := Dictionary new.
		self do: [ :each | | key group |
			key := keyBlock value: each.
			group := dict at: key ifAbsent: [nil].
			group isNil ifTrue: [
				group := OrderedCollection new.
				dict at: key put: group].
			group add: each].
		^dict
	]


	"--- terminals: converting ---"

	asArray [
		^self asOrderedCollection asArray
	]


	asOrderedCollection [
		| result |

		result := OrderedCollection new.
		self do: [ :each | result add: each].
		^result
	]


	asSet [
		| result |

		result := Set new.
		self do: [ :each | result add: each].
		^result
	]


	asBag [
		| result |

		result := Bag new.
		self do: [ :each | result add: each].
		^result
	]


	toDictionary: keyBlock [
		"Key is keyBlock value: element, value is the element itself."
		| dict |

		dict := Dictionary new.
		self do: [ :each | dict at: (keyBlock value: each) put: each].
		^dict
	]


	toDictionary: keyBlock values: valueBlock [
		| dict |

		dict := Dictionary new.
		self do: [ :each | dict at: (keyBlock value: each) put: (valueBlock value: each)].
		^dict
	]


	"--- terminals: accessing ---"

	first [
		self do: [ :each | ^each].
		^self error: 'first: sequence is empty'
	]


	firstOrNil [
		self do: [ :each | ^each].
		^nil
	]


	firstOrDefault [
		"LINQ alias of #firstOrNil."
		^self firstOrNil
	]


	"--- terminals: testing ---"

	any [
		self do: [ :each | ^true].
		^false
	]


	all: aBlock [
		self do: [ :each | (aBlock value: each) ifFalse: [^false]].
		^true
	]


	allSatisfy: aBlock [
		^self all: aBlock
	]


	none: aBlock [
		self do: [ :each | (aBlock value: each) ifTrue: [^false]].
		^true
	]


	includes: anObject [
		self do: [ :each | each = anObject ifTrue: [^true]].
		^false
	]


	"--- terminals: counting and reducing ---"

	count [
		| tally |

		tally := 0.
		self do: [ :each | tally := tally + 1].
		^tally
	]


	count: aBlock [
		| tally |

		tally := 0.
		self do: [ :each | (aBlock value: each) ifTrue: [tally := tally + 1]].
		^tally
	]


	sum: aBlock [
		| total any |

		total := 0.
		any := false.
		self do: [ :each | | v |
			v := aBlock value: each.
			any
				ifTrue: [total := total + v]
				ifFalse: [total := v. any := true]].
		any ifFalse: [^self error: 'sum: sequence is empty'].
		^total
	]


	sum [
		^self sum: [ :each | each]
	]


	max [
		| result any |

		any := false.
		self do: [ :each |
			any
				ifTrue: [each > result ifTrue: [result := each]]
				ifFalse: [result := each. any := true]].
		any ifFalse: [^self error: 'max: sequence is empty'].
		^result
	]


	min [
		| result any |

		any := false.
		self do: [ :each |
			any
				ifTrue: [each < result ifTrue: [result := each]]
				ifFalse: [result := each. any := true]].
		any ifFalse: [^self error: 'min: sequence is empty'].
		^result
	]


	average [
		| total tally |

		total := 0.
		tally := 0.
		self do: [ :each | total := total + each. tally := tally + 1].
		tally = 0 ifTrue: [^self error: 'average: sequence is empty'].
		^total / tally
	]


	aggregate: initialValue into: aBlock [
		"Alias of #inject:into: (inherited from Iterator), same argument order:
		 the seed value first, then the binary block."
		^self inject: initialValue into: aBlock
	]


	"--- printing: MUST NOT iterate, so inspecting cannot trigger effects ---"

	printOn: aStream [
		aStream nextPutAll: 'a LazySequence'
	]


	"--- private helpers shared by the stages (inherited) ---"

	lazyMaterialize: aSource [
		"Drain anything that answers #do: into a fresh indexable Array."
		| result |

		result := OrderedCollection new.
		aSource do: [ :each | result add: each].
		^result asArray
	]


	lazySort: anArray by: aBlock [
		"In-place stable merge sort over anArray using the two-arg sort block."
		| n temp |

		n := anArray size.
		n <= 1 ifTrue: [^anArray].
		temp := Array new: n.
		self lazyMergeSortFrom: 1 to: n on: anArray using: temp by: aBlock.
		^anArray
	]


	lazyMergeSortFrom: low to: high on: anArray using: temp by: aBlock [
		| mid i j k |

		high <= low ifTrue: [^self].
		mid := low + high // 2.
		self lazyMergeSortFrom: low to: mid on: anArray using: temp by: aBlock.
		self lazyMergeSortFrom: mid + 1 to: high on: anArray using: temp by: aBlock.
		i := low.
		j := mid + 1.
		k := low.
		[k <= high] whileTrue: [
			(i > mid)
				ifTrue: [temp at: k put: (anArray at: j). j := j + 1]
				ifFalse: [
					(j > high)
						ifTrue: [temp at: k put: (anArray at: i). i := i + 1]
						ifFalse: [
							(aBlock value: (anArray at: i) value: (anArray at: j))
								ifTrue: [temp at: k put: (anArray at: i). i := i + 1]
								ifFalse: [temp at: k put: (anArray at: j). j := j + 1]]].
			k := k + 1].
		low to: high do: [ :m | anArray at: m put: (temp at: m)].
	]

]


"====================== intermediate stage classes ======================"

LazyCollectStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		upstream do: [ :each | aBlock value: (block value: each)].
	]

]


LazySelectStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		upstream do: [ :each | (block value: each) ifTrue: [aBlock value: each]].
	]

]


LazyRejectStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		upstream do: [ :each | (block value: each) ifFalse: [aBlock value: each]].
	]

]


LazyFlatCollectStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		upstream do: [ :each | (block value: each) do: [ :sub | aBlock value: sub]].
	]

]


LazyTakeStage := LazySequence [

	| upstream count |


	class upstream: anUpstream count: anInteger [
		^self new setUpstream: anUpstream count: anInteger
	]


	setUpstream: anUpstream count: anInteger [
		upstream := anUpstream.
		count := anInteger.
		^self
	]


	do: aBlock [
		"seen is per-iteration state, created here so re-iteration is clean. In a
		 push model the upstream produces a value before we can reject it, so the
		 quota is checked AFTER yielding: that way an upstream that computes its
		 elements (a collect: block, say) runs EXACTLY count times, not count + 1.
		 The ^self exits this #do: and unwinds the upstream loop (early termination
		 via non-local return)."
		| seen |

		count <= 0 ifTrue: [^self].
		seen := 0.
		upstream do: [ :each |
			aBlock value: each.
			seen := seen + 1.
			seen >= count ifTrue: [^self]].
	]

]


LazySkipStage := LazySequence [

	| upstream count |


	class upstream: anUpstream count: anInteger [
		^self new setUpstream: anUpstream count: anInteger
	]


	setUpstream: anUpstream count: anInteger [
		upstream := anUpstream.
		count := anInteger.
		^self
	]


	do: aBlock [
		| seen |

		seen := 0.
		upstream do: [ :each |
			seen >= count
				ifTrue: [aBlock value: each]
				ifFalse: [seen := seen + 1]].
	]

]


LazyTakeWhileStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		"Non-local return stops the upstream at the first failing element."
		upstream do: [ :each |
			(block value: each) ifFalse: [^self].
			aBlock value: each].
	]

]


LazySkipWhileStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		| skipping |

		skipping := true.
		upstream do: [ :each |
			skipping
				ifTrue: [
					(block value: each) ifFalse: [
						skipping := false.
						aBlock value: each]]
				ifFalse: [aBlock value: each]].
	]

]


LazyDistinctStage := LazySequence [

	| upstream |


	class upstream: anUpstream [
		^self new setUpstream: anUpstream
	]


	setUpstream: anUpstream [
		upstream := anUpstream.
		^self
	]


	do: aBlock [
		"The seen Set is per-iteration state. Newness is detected by the Set
		 growing (using its own = based hashing), which avoids a linear scan."
		| seen |

		seen := Set new.
		upstream do: [ :each | | before |
			before := seen size.
			seen add: each.
			seen size > before ifTrue: [aBlock value: each]].
	]

]


LazyZipStage := LazySequence [

	| upstream other |


	class upstream: anUpstream other: aCollection [
		^self new setUpstream: anUpstream other: aCollection
	]


	setUpstream: anUpstream other: aCollection [
		upstream := anUpstream.
		other := aCollection.
		^self
	]


	do: aBlock [
		"The other side is materialized to an Array and indexed by a per-iteration
		 counter. Stops at the shorter side: if upstream is shorter its #do: just
		 ends; if other is shorter we non-local return once the index runs past it."
		| others index |

		others := self lazyMaterialize: other.
		index := 0.
		upstream do: [ :each |
			index := index + 1.
			index > others size ifTrue: [^self].
			aBlock value: (each -> (others at: index))].
	]

]


LazyWithIndexCollectStage := LazySequence [

	| upstream block |


	class upstream: anUpstream block: aBlock [
		^self new setUpstream: anUpstream block: aBlock
	]


	setUpstream: anUpstream block: aBlock [
		upstream := anUpstream.
		block := aBlock.
		^self
	]


	do: aBlock [
		| index |

		index := 0.
		upstream do: [ :each |
			index := index + 1.
			aBlock value: (block value: each value: index)].
	]

]


LazySortStage := LazySequence [

	| upstream sortBlock |


	class upstream: anUpstream sortBlock: aBlock [
		^self new setUpstream: anUpstream sortBlock: aBlock
	]


	setUpstream: anUpstream sortBlock: aBlock [
		upstream := anUpstream.
		sortBlock := aBlock.
		^self
	]


	do: aBlock [
		"Materializes a FRESH copy of the upstream each iteration, sorts it with
		 the private merge sort, then pushes the sorted elements downstream. The
		 array is per-iteration state, so re-iteration is deterministic."
		| array |

		array := self lazyMaterialize: upstream.
		self lazySort: array by: sortBlock.
		array do: aBlock.
	]

]
