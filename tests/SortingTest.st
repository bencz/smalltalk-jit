"SortingTest: the in-place stable merge sort (sort / sort:) and the copying
 variants (sorted / sorted:), plus isSorted / isSorted:. Covers empty, single,
 already-sorted, reversed and duplicate inputs; a stability check with a key
 sort block; and String, Array and OrderedCollection receivers."

[
	| t arr oc data sortedVals sc |
	t := TestRun named: 'Sorting'.

	"---- isSorted / isSorted: ----"
	t check: 'isSorted empty' that: [ (Array new: 0) isSorted ].
	t check: 'isSorted single' that: [ (Array with: 5) isSorted ].
	t check: 'isSorted ascending' that: [ #(1 2 3 4 5) isSorted ].
	t check: 'isSorted rejects unsorted' that: [ #(1 3 2) isSorted not ].
	t check: 'isSorted: descending block' that: [ #(5 4 3 2 1) isSorted: [:a :b | a >= b] ].
	t check: 'isSorted: rejects wrong order' that: [ (#(1 2 3) isSorted: [:a :b | a >= b]) not ].
	t check: 'isSorted with duplicates' that: [ #(1 1 2 2 3) isSorted ].

	"---- sort (in place, ascending) over the edge cases ----"
	t check: 'sort empty' equals: 0 is: [ (Array new: 0) sort size ].
	t check: 'sort single' equals: #(5) is: [ (Array with: 5) sort ].
	t check: 'sort already sorted' equals: #(1 2 3 4 5) is: [ #(1 2 3 4 5) copy sort ].
	t check: 'sort reversed' equals: #(1 2 3 4 5) is: [ #(5 4 3 2 1) copy sort ].
	t check: 'sort with duplicates' equals: #(1 1 2 3 3) is: [ #(3 1 2 3 1) copy sort ].
	t check: 'sort returns the receiver' that: [ arr := #(2 1) copy. (arr sort) == arr ].

	"---- sort: with a descending block ----"
	t check: 'sort: descending' equals: #(5 4 3 2 1) is: [ #(1 5 2 4 3) copy sort: [:a :b | a >= b] ].

	"---- stability: equal keys keep their input order ----"
	data := OrderedCollection new.
	data add: 2 -> 'a'; add: 1 -> 'b'; add: 2 -> 'c'; add: 1 -> 'd'; add: 2 -> 'e'.
	arr := data asArray.
	arr sort: [:x :y | x key <= y key].
	sortedVals := arr collect: [:assoc | assoc value].
	t check: 'merge sort is stable on equal keys' equals: #('b' 'd' 'a' 'c' 'e') is: [ sortedVals ].

	"---- String sort in place ----"
	t check: 'String sort in place' equals: 'abcd' is: [ 'dbca' copy sort ].
	t check: 'String sort with duplicates' equals: 'aabbcc' is: [ 'bcabca' copy sort ].

	"---- OrderedCollection sort in place ----"
	oc := OrderedCollection withAll: #(3 1 4 1 5 9 2 6).
	oc sort.
	t check: 'OrderedCollection sort in place' equals: #(1 1 2 3 4 5 6 9) is: [ oc asArray ].

	"---- Array sort in place ----"
	t check: 'Array sort in place' equals: #(1 2 3 4 5) is: [ #(3 5 1 4 2) copy sort ].

	"---- sorted / sorted: leave the receiver untouched and answer an Array ----"
	oc := OrderedCollection withAll: #(3 1 2).
	sc := oc sorted.
	t check: 'sorted answers an Array' that: [ sc isKindOf: Array ].
	t check: 'sorted result is ordered' equals: #(1 2 3) is: [ sc ].
	t check: 'sorted leaves the receiver unchanged' equals: #(3 1 2) is: [ oc asArray ].
	t check: 'sorted: descending' equals: #(5 4 3 2 1) is: [ #(1 2 3 4 5) sorted: [:a :b | a >= b] ].
	t check: 'sorted: over an Interval' equals: #(1 2 3 4 5) is: [ (5 to: 1 by: -1) sorted ].

	^t report
]
