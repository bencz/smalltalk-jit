"CollectionEnrichmentTest: the non-sorting Collection / SequenceableCollection
 protocol added in wave 2: flatCollect:/gather:, allSatisfy:/noneSatisfy:,
 includesAll:/includesAny:, groupBy:, sum:/max:/min: by block,
 detect:ifFound:ifNone:, select:thenCollect:/reject:thenCollect:, collect:as:,
 asDictionary, union:/intersection:/difference:, first:/last:/allBut*,
 endsWith:, with:do:, doWithIndex:/withIndexDo:, atAll:put:/atAllPut:,
 copyWithout:, copyReplaceAll:with:, and the empty-collection guards on
 max/min/average."

[
	| t g d oc s arr count seen id |
	t := TestRun named: 'CollectionEnrichment'.

	"---- flatCollect: / gather: ----"
	t check: 'flatCollect over Array' equals: #(1 10 2 20 3 30) is: [
		(#(1 2 3) flatCollect: [:x | Array with: x with: x * 10]) asArray ].
	t check: 'gather: is an alias of flatCollect:' equals: #(1 1 2 2) is: [
		(#(1 2) gather: [:x | Array with: x with: x]) asArray ].
	t check: 'flatCollect over a Set (multiset preserved)' equals: #(1 1 2 2 3 3) is: [
		((Set withAll: #(1 2 3)) flatCollect: [:x | Array with: x with: x])
			asSortedCollection asArray ].
	t check: 'flatCollect over Dictionary values' equals: 63 is: [
		d := Dictionary new. d at: #a put: 10; at: #b put: 20; at: #c put: 30.
		(d flatCollect: [:v | Array with: v with: 1]) inject: 0 into: [:acc :e | acc + e] ].

	"---- allSatisfy: / noneSatisfy: ----"
	t check: 'allSatisfy: true' that: [ #(2 4 6) allSatisfy: [:x | x even] ].
	t check: 'allSatisfy: false' that: [ (#(2 4 5) allSatisfy: [:x | x even]) not ].
	t check: 'allSatisfy: empty is true' that: [ (Array new: 0) allSatisfy: [:x | false] ].
	t check: 'noneSatisfy: true' that: [ #(1 3 5) noneSatisfy: [:x | x even] ].
	t check: 'noneSatisfy: false' that: [ (#(1 3 4) noneSatisfy: [:x | x even]) not ].

	"---- includesAll: / includesAny: ----"
	t check: 'includesAll: present' that: [ #(1 2 3 4) includesAll: #(2 3) ].
	t check: 'includesAll: missing one' that: [ (#(1 2 3 4) includesAll: #(2 9)) not ].
	t check: 'includesAny: one present' that: [ #(1 2 3 4) includesAny: #(9 3) ].
	t check: 'includesAny: none present' that: [ (#(1 2 3 4) includesAny: #(9 8)) not ].

	"---- groupBy: ----"
	g := #(1 2 3 4 5 6) groupBy: [:x | x even].
	t check: 'groupBy: is a Dictionary' that: [ g isKindOf: Dictionary ].
	t check: 'groupBy: even bucket' equals: #(2 4 6) is: [ (g at: true) asArray ].
	t check: 'groupBy: odd bucket' equals: #(1 3 5) is: [ (g at: false) asArray ].

	"---- sum: / max: / min: by block ----"
	t check: 'sum: squares' equals: 14 is: [ #(1 2 3) sum: [:x | x * x] ].
	t check: 'sum: empty is zero' equals: 0 is: [ (Array new: 0) sum: [:x | x] ].
	t check: 'max: by negation' equals: 1 is: [ (#(1 2 3) max: [:x | x negated]) negated ].
	t check: 'min: by negation' equals: 3 is: [ (#(1 2 3) min: [:x | x negated]) negated ].
	t expect: 'max: empty raises' raises: Error in: [ (Array new: 0) max: [:x | x] ].
	t expect: 'min: empty raises' raises: Error in: [ (Array new: 0) min: [:x | x] ].

	"---- empty-collection guards on the plain reducers ----"
	t expect: 'max of empty raises' raises: Error in: [ (Array new: 0) max ].
	t expect: 'min of empty raises' raises: Error in: [ (Array new: 0) min ].
	t expect: 'average of empty raises' raises: Error in: [ (Array new: 0) average ].
	t check: 'sum of empty stays 0' equals: 0 is: [ (Array new: 0) sum ].

	"---- detect:ifFound:ifNone: ----"
	t check: 'detect:ifFound:ifNone: found' equals: 200 is: [
		#(1 2 3 4) detect: [:x | x even] ifFound: [:x | x * 100] ifNone: [0] ].
	t check: 'detect:ifFound:ifNone: none' equals: 0 is: [
		#(1 3 5) detect: [:x | x even] ifFound: [:x | x] ifNone: [0] ].

	"---- select:thenCollect: / reject:thenCollect: ----"
	t check: 'select:thenCollect:' equals: #(20 40) is: [
		(#(1 2 3 4) select: [:x | x even] thenCollect: [:x | x * 10]) asArray ].
	t check: 'reject:thenCollect:' equals: #(10 30) is: [
		(#(1 2 3 4) reject: [:x | x even] thenCollect: [:x | x * 10]) asArray ].

	"---- collect:as: ----"
	t check: 'collect:as: Array' equals: #(2 4 6) is: [ #(1 2 3) collect: [:x | x * 2] as: Array ].
	t check: 'collect:as: Set size' equals: 2 is: [ (#(1 1 2 2) collect: [:x | x] as: Set) size ].

	"---- asDictionary from Associations ----"
	d := (Array with: 1 -> 10 with: 2 -> 20) asDictionary.
	t check: 'asDictionary builds a Dictionary' that: [ d isKindOf: Dictionary ].
	t check: 'asDictionary keeps first pair' equals: 10 is: [ d at: 1 ].
	t check: 'asDictionary keeps second pair' equals: 20 is: [ d at: 2 ].

	"---- set operations across mixed collection types ----"
	t check: 'union of Array and Set' equals: #(1 2 3 4 5) is: [
		(#(1 2 3) union: (Set withAll: #(3 4 5))) asSortedCollection asArray ].
	t check: 'union answers a Set' that: [ (#(1 2) union: #(3 4)) isKindOf: Set ].
	t check: 'intersection' equals: #(3 4) is: [
		(#(1 2 3 4) intersection: #(3 4 5 6)) asSortedCollection asArray ].
	t check: 'difference' equals: #(1 2) is: [
		(#(1 2 3 4) difference: #(3 4 5 6)) asSortedCollection asArray ].
	t check: 'intersection dedups duplicates' equals: 1 is: [
		(#(2 2 2) intersection: #(2)) size ].

	"---- first: / last: / allBut* ----"
	t check: 'first: n' equals: #(1 2) is: [ #(1 2 3 4 5) first: 2 ].
	t check: 'last: n' equals: #(4 5) is: [ #(1 2 3 4 5) last: 2 ].
	t check: 'allButFirst' equals: #(2 3 4 5) is: [ #(1 2 3 4 5) allButFirst ].
	t check: 'allButLast' equals: #(1 2 3 4) is: [ #(1 2 3 4 5) allButLast ].
	t check: 'allButFirst: n' equals: #(3 4 5) is: [ #(1 2 3 4 5) allButFirst: 2 ].
	t check: 'allButLast: n' equals: #(1 2 3) is: [ #(1 2 3 4 5) allButLast: 2 ].
	t check: 'first: on a String' equals: 'he' is: [ 'hello' first: 2 ].

	"---- endsWith: ----"
	t check: 'endsWith: Array true' that: [ #(1 2 3 4) endsWith: #(3 4) ].
	t check: 'endsWith: Array false' that: [ (#(1 2 3 4) endsWith: #(3 5)) not ].
	t check: 'endsWith: String true' that: [ 'hello' endsWith: 'llo' ].
	t check: 'endsWith: longer than self false' that: [ (#(1 2) endsWith: #(0 1 2)) not ].
	t check: 'endsWith: empty false' that: [ ('hello' endsWith: '') not ].

	"---- with:do: ----"
	count := 0.
	#(1 2 3) with: #(10 20 30) do: [:a :b | count := count + (a * b)].
	t check: 'with:do: pairs' equals: 140 is: [ count ].

	"---- doWithIndex: / withIndexDo: (element then index) ----"
	oc := OrderedCollection new.
	#(10 20 30) doWithIndex: [:e :i | oc add: (Array with: e with: i)].
	t check: 'doWithIndex: first pair' equals: #(10 1) is: [ oc first ].
	t check: 'doWithIndex: last pair' equals: #(30 3) is: [ oc last ].
	oc := OrderedCollection new.
	#(10 20 30) withIndexDo: [:e :i | oc add: e + i].
	t check: 'withIndexDo: element plus index' equals: #(11 22 33) is: [ oc asArray ].

	"---- atAll:put: / atAllPut: ----"
	arr := #(1 2 3 4 5) copy.
	arr atAllPut: 9.
	t check: 'atAllPut:' equals: #(9 9 9 9 9) is: [ arr ].
	arr := #(1 2 3 4 5) copy.
	arr atAll: #(1 3 5) put: 0.
	t check: 'atAll:put:' equals: #(0 2 0 4 0) is: [ arr ].

	"---- copyWithout: ----"
	t check: 'copyWithout: Array' equals: #(1 3 1) is: [ #(1 2 3 2 1) copyWithout: 2 ].
	t check: 'copyWithout: String' equals: 'heo' is: [ 'hello' copyWithout: $l ].
	t check: 'copyWithout: absent leaves a copy' equals: #(1 2 3) is: [ #(1 2 3) copyWithout: 9 ].

	"---- copyReplaceAll:with: (start / end / absent / adjacent) ----"
	t check: 'copyReplaceAll: at start' equals: #(9 3 4) is: [ #(1 2 3 4) copyReplaceAll: #(1 2) with: #(9) ].
	t check: 'copyReplaceAll: at end' equals: #(1 2 9) is: [ #(1 2 3 4) copyReplaceAll: #(3 4) with: #(9) ].
	t check: 'copyReplaceAll: absent' equals: #(1 2 3) is: [ #(1 2 3) copyReplaceAll: #(9) with: #(0) ].
	t check: 'copyReplaceAll: adjacent occurrences' equals: #(9 9) is: [ #(1 1 1 1) copyReplaceAll: #(1 1) with: #(9) ].
	t check: 'copyReplaceAll: grows the result' equals: #(1 9 9 3 9 9 5) is: [
		#(1 2 3 2 5) copyReplaceAll: #(2) with: #(9 9) ].
	t check: 'copyReplaceAll: on String' equals: 'aYYbYYc' is: [ 'aXbXc' copyReplaceAll: 'X' with: 'YY' ].
	t check: 'copyReplaceAll: empty pattern copies self' equals: 'abc' is: [ 'abc' copyReplaceAll: '' with: 'z' ].

	^t report
]
