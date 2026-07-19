"Tests for LazySequence, the LINQ-to-Objects style deferred pipeline (Wave 4).
 Covers observable laziness, short-circuit evaluation, chaining, the stateful
 stages, barriers, terminals, re-iteration, many source kinds, the LINQ
 aliases, and that unwind handlers run on early termination."

"A source whose #do: wraps the iteration in an ensure:. Used to prove that a
 short-circuit (a non-local return out of take:) still runs cleanup handlers."
LazyEnsureSource := Object [

	| cleaned |


	cleaned [
		^cleaned
	]


	do: aBlock [
		cleaned := false.
		[1 to: 100 do: [ :i | aBlock value: i]] ensure: [cleaned := true].
	]

]


"---- 1. observable laziness: nothing runs before a terminal ----"
[
	| counter seq |

	LazyRun := TestRun named: 'LazySequence'.

	counter := 0.
	seq := (1 to: 10) asLazy collect: [ :x | counter := counter + 1. x].
	LazyRun check: 'no terminal means no effect' equals: 0 is: [counter].
	seq asArray.
	LazyRun check: 'asArray forces the whole source' equals: 10 is: [counter].
	seq asArray.
	LazyRun check: 're-running asArray re-runs the effect' equals: 20 is: [counter].
]


"---- 2. short-circuit: only as much of the source as needed is touched ----"
[
	| calls |

	calls := 0.
	((1 to: 1000000) asLazy collect: [ :x | calls := calls + 1. x]) take: 3; yourself.
	LazyRun check: 'take: 3 evaluates collect exactly 3 times'
		equals: 3
		is: [
			| c |
			c := 0.
			(((1 to: 1000000) asLazy collect: [ :x | c := c + 1. x]) take: 3) asArray.
			c].

	LazyRun check: 'first evaluates collect exactly once'
		equals: 1
		is: [
			| c |
			c := 0.
			((1 to: 1000000) asLazy collect: [ :x | c := c + 1. x]) first.
			c].

	LazyRun check: 'anySatisfy: stops at the first match'
		equals: 5
		is: [
			| c |
			c := 0.
			((1 to: 1000000) asLazy collect: [ :x | c := c + 1. x]) anySatisfy: [ :x | x >= 5].
			c].
]


"---- 3. chaining select+collect+skip+take matches the eager equivalent ----"
[
	| base filtered mapped skipped taken eager |

	base := (1 to: 20) asLazy.
	filtered := base select: [ :x | x even].
	mapped := filtered collect: [ :x | x * x].
	skipped := mapped skip: 1.
	taken := skipped take: 3.
	eager := ((((1 to: 20) select: [ :x | x even]) collect: [ :x | x * x]) asArray) copyFrom: 2 to: 4.
	LazyRun check: 'select+collect+skip+take equals eager' equals: eager is: [taken asArray].

	LazyRun check: 'reject keeps the complement' equals: #(1 3 5) is: [
		((1 to: 6) asLazy reject: [ :x | x even]) asArray].

	LazyRun check: 'deep 5-stage chain' equals: #(20 24) is: [
		| s |
		s := ((((( 1 to: 100) asLazy)
			select: [ :x | x even])
			collect: [ :x | x * 2])
			skip: 4)
			take: 2.
		s asArray].
]


"---- 4. stateful stages: distinct, zip:, takeWhile:/skipWhile:, flatCollect: ----"
[
	LazyRun check: 'distinct removes duplicates keeping first order' equals: #(1 2 3) is: [
		(#(1 1 2 3 3 3 2 1) asLazy distinct) asArray].

	LazyRun check: 'takeWhile: stops at first failure' equals: #(1 2 3) is: [
		((1 to: 10) asLazy takeWhile: [ :x | x < 4]) asArray].

	LazyRun check: 'skipWhile: drops the leading run' equals: #(4 5 6) is: [
		((1 to: 6) asLazy skipWhile: [ :x | x < 4]) asArray].

	LazyRun check: 'flatCollect: flattens Arrays one level' equals: #(1 10 2 20 3 30) is: [
		(#(1 2 3) asLazy flatCollect: [ :x | Array with: x with: x * 10]) asArray].

	LazyRun check: 'flatCollect: flattens OrderedCollections' equals: #(1 1 2 2) is: [
		(#(1 2) asLazy flatCollect: [ :x | OrderedCollection with: x with: x]) asArray].

	LazyRun check: 'zip: stops at shorter (other side)' equals: 2 is: [
		(#(1 2 3) asLazy zip: #(10 20)) count].

	LazyRun check: 'zip: stops at shorter (upstream side)' equals: 3 is: [
		(#(1 2 3) asLazy zip: #(10 20 30 40)) count].

	LazyRun check: 'zip: pairs by position' equals: #(110 220 330) is: [
		((#(1 2 3) asLazy zip: #(10 20 30)) collect: [ :p | (p key * 100) + p value]) asArray].

	LazyRun check: 'withIndexCollect: passes 1-based index' equals: #(11 22 33) is: [
		(#(10 20 30) asLazy withIndexCollect: [ :each :i | each + i]) asArray].
]


"---- 5. barriers: sorted, sorted:, orderBy:, orderByDescending:, groupBy: ----"
[
	LazyRun check: 'sorted ascending' equals: #(1 2 3 4 5) is: [
		(#(3 1 4 1 5 2) asLazy distinct sorted) asArray].

	LazyRun check: 'sorted: with a custom block (descending)' equals: #(5 4 3 2 1) is: [
		(#(3 1 4 5 2) asLazy sorted: [ :a :b | a >= b]) asArray].

	LazyRun check: 'sorted is stable and complete beyond small sizes' equals: (1 to: 30) asArray is: [
		((30 to: 1 by: -1) asLazy sorted) asArray].

	LazyRun check: 'orderBy: key ascending' equals: #(3 2 1) is: [
		(#(1 2 3) asLazy orderBy: [ :x | x negated]) asArray].

	LazyRun check: 'orderByDescending:' equals: #(4 3 2 1) is: [
		(#(2 4 1 3) asLazy orderByDescending: [ :x | x]) asArray].

	LazyRun check: 'groupBy: partitions and keeps order (even group)' equals: #(2 4 6) is: [
		((#(1 2 3 4 5 6) asLazy groupBy: [ :x | x \\ 2]) at: 0) asArray].

	LazyRun check: 'groupBy: partitions and keeps order (odd group)' equals: #(1 3 5) is: [
		((#(1 2 3 4 5 6) asLazy groupBy: [ :x | x \\ 2]) at: 1) asArray].

	LazyRun check: 'groupBy: has one entry per key' equals: 2 is: [
		(#(1 2 3 4 5 6) asLazy groupBy: [ :x | x \\ 2]) size].
]


"---- 6. terminals: dictionaries, reducers, quantifiers, empty behavior ----"
[
	LazyRun check: 'toDictionary: maps key to element' equals: 2 is: [
		(#(1 2 3) asLazy toDictionary: [ :x | x * 10]) at: 20].

	LazyRun check: 'toDictionary:values: maps both sides' equals: 9 is: [
		(#(1 2 3) asLazy toDictionary: [ :x | x] values: [ :x | x * x]) at: 3].

	LazyRun check: 'sum' equals: 15 is: [(1 to: 5) asLazy sum].
	LazyRun check: 'sum: with a mapping block' equals: 14 is: [(#(1 2 3) asLazy) sum: [ :x | x * x]].
	LazyRun check: 'count' equals: 5 is: [(1 to: 5) asLazy count].
	LazyRun check: 'count: with a predicate' equals: 2 is: [(1 to: 5) asLazy count: [ :x | x even]].
	LazyRun check: 'average' equals: 5 is: [#(2 4 6 8) asLazy average].
	LazyRun check: 'max' equals: 9 is: [#(3 1 4 1 5 9 2) asLazy max].
	LazyRun check: 'min' equals: 1 is: [#(3 1 4 1 5 9 2) asLazy min].
	LazyRun check: 'includes: present' that: [(1 to: 10) asLazy includes: 5].
	LazyRun check: 'includes: absent' equals: false is: [(1 to: 10) asLazy includes: 50].

	LazyRun check: 'all: true' that: [#(2 4 6) asLazy all: [ :x | x even]].
	LazyRun check: 'all: false' equals: false is: [#(2 4 5) asLazy all: [ :x | x even]].
	LazyRun check: 'none: true' that: [#(1 3 5) asLazy none: [ :x | x even]].
	LazyRun check: 'none: false' equals: false is: [#(1 3 4) asLazy none: [ :x | x even]].
	LazyRun check: 'any on non-empty' that: [#(7) asLazy any].
	LazyRun check: 'any on empty' equals: false is: [(1 to: 0) asLazy any].

	LazyRun check: 'firstOrNil on empty is nil' equals: nil is: [(1 to: 0) asLazy firstOrNil].
	LazyRun check: 'first on non-empty' equals: 1 is: [(1 to: 5) asLazy first].
	LazyRun expect: 'first on empty raises' raises: Error in: [(1 to: 0) asLazy first].
	LazyRun expect: 'sum on empty raises' raises: Error in: [(1 to: 0) asLazy sum].
	LazyRun expect: 'max on empty raises' raises: Error in: [(1 to: 0) asLazy max].
	LazyRun expect: 'min on empty raises' raises: Error in: [(1 to: 0) asLazy min].
	LazyRun expect: 'average on empty raises' raises: Error in: [(1 to: 0) asLazy average].
]


"---- 7. re-iteration: the same pipeline gives the same result each time ----"
[
	| seq distinctSeq |

	seq := ((1 to: 10) asLazy select: [ :x | x even]) take: 2.
	LazyRun check: 'take: pipeline first run' equals: #(2 4) is: [seq asArray].
	LazyRun check: 'take: pipeline second run identical' equals: #(2 4) is: [seq asArray].

	distinctSeq := #(1 1 2 2 3 1) asLazy distinct.
	LazyRun check: 'distinct pipeline first run' equals: #(1 2 3) is: [distinctSeq asArray].
	LazyRun check: 'distinct pipeline second run identical' equals: #(1 2 3) is: [distinctSeq asArray].
]


"---- 8. many source kinds and the LINQ aliases ----"
[
	| dict |

	LazyRun check: 'source Array' equals: #(2 4 6) is: [
		(#(1 2 3) asLazy collect: [ :x | x * 2]) asArray].

	LazyRun check: 'source OrderedCollection' equals: #(2 4 6) is: [
		((OrderedCollection withAll: #(1 2 3)) asLazy collect: [ :x | x * 2]) asArray].

	LazyRun check: 'source Interval' equals: #(1 2 3 4 5) is: [
		((1 to: 5) asLazy) asArray].

	LazyRun check: 'source Set (order-independent via sorted)' equals: #(1 2 3) is: [
		((Set withAll: #(3 1 2 1)) asLazy sorted) asArray].

	LazyRun check: 'source Set (order-independent via asSet size)' equals: 3 is: [
		((Set withAll: #(3 1 2 1)) asLazy) asSet size].

	dict := Dictionary new.
	dict at: #a put: 10.
	dict at: #b put: 20.
	dict at: #c put: 30.
	LazyRun check: 'source Dictionary iterates values' equals: #(10 20 30) is: [
		(dict asLazy sorted) asArray].

	LazyRun check: 'source CollectionStream (read stream)' equals: #(10 20 30) is: [
		((CollectionStream on: #(1 2 3)) asLazy collect: [ :x | x * 10]) asArray].

	LazyRun check: 'asBag terminal counts occurrences' equals: 2 is: [
		(#(1 1 2 3) asLazy asBag) occurrencesOf: 1].

	LazyRun check: 'where: is the filter alias' equals: #(2 4 6) is: [
		((1 to: 6) asLazy where: [ :x | x even]) asArray].

	LazyRun check: 'selectMany: is the flatCollect alias' equals: #(1 1 2 2 3 3) is: [
		(#(1 2 3) asLazy selectMany: [ :x | Array with: x with: x]) asArray].

	LazyRun check: 'aggregate:into: is the inject:into: alias' equals: 15 is: [
		(1 to: 5) asLazy aggregate: 0 into: [ :acc :x | acc + x]].

	LazyRun check: 'printOn: does not iterate' equals: 'a LazySequence' is: [
		((1 to: 1000000) asLazy collect: [ :x | x]) printString].
]


"---- 9. unwind: ensure: cleanup runs even when take: short-circuits ----"
[
	| src res continued |

	src := LazyEnsureSource new.
	res := ((LazySequence on: src) take: 2) asArray.
	continued := 42.
	LazyRun check: 'short-circuit yields the first N' equals: #(1 2) is: [res].
	LazyRun check: 'ensure: cleanup ran despite non-local return' that: [src cleaned].
	LazyRun check: 'program continues normally after short-circuit' equals: 42 is: [continued].
]


[ ^LazyRun report ]
