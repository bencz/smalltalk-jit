"Regression tests for the collection/number protocol added and
 fixed alongside the samples: enumeration helpers, conversions,
 reverse, concatenation, copyFrom:, the insert:beforeIndex: growth
 fix, SortedCollection, and the Number math functions."

"---- enumeration on non-OrderedCollections ----"
[
	Assert true: ((1 to: 10) select: [:x | x even]) asArray = #(2 4 6 8 10).
	Assert true: (#(5 3 8 1 9) reject: [:x | x > 4]) asArray = #(3 1).
	Assert true: ((Set new add: 1; add: 2; add: 3; yourself) collect: [:x | x]) size = 3.
	Assert true: ((1 to: 10) count: [:x | x even]) = 5.
	Assert true: (#(3 5 8 9) detect: [:x | x even] ifNone: [0]) = 8.
]

"---- reducing and converting ----"
[
	Assert true: #(1 2 3 4 5) sum = 15.
	Assert true: #(3 1 4 1 5 9 2 6) max = 9.
	Assert true: #(3 1 4 1 5 9 2 6) min = 1.
	Assert true: #(2 4 6) average = 4.
	Assert true: (#(5 3 8) asOrderedCollection) = (OrderedCollection with: 5 with: 3 with: 8).
	Assert true: ((1 to: 4) asArray) = #(1 2 3 4).
]

"---- reverse and with:collect: ----"
[
	Assert true: #(1 2 3) reverse = #(3 2 1).
	Assert true: ((1 to: 5) reverse) = #(5 4 3 2 1).
	Assert true: (OrderedCollection withAll: #(1 2 3)) reverse asArray = #(3 2 1).
	Assert true: (#(1 2 3) with: #(10 20 30) collect: [:a :b | a + b]) = #(11 22 33).
	Assert true: 'hello' asUppercase = 'HELLO'.
]

"---- do:separatedBy: builds a nicely formatted string ----"
[
	| s |
	s := CollectionStream with: (String new: 0).
	#(1 2 3) do: [:x | s nextPutAll: x printString] separatedBy: [s nextPutAll: ', '].
	Assert true: s contents = '1, 2, 3'.
]

"---- concatenation and copyFrom: on growable / interval ----"
[
	Assert true: ((OrderedCollection withAll: #(1 2)) , (OrderedCollection withAll: #(3 4))) asArray = #(1 2 3 4).
	Assert true: ((1 to: 3) , (4 to: 5)) = #(1 2 3 4 5).
	Assert true: ((OrderedCollection withAll: #(10 20 30 40)) copyFrom: 2 to: 3) asArray = #(20 30).
	Assert true: ((1 to: 10) copyFrom: 3 to: 5) = #(3 4 5).
]

"---- insert:beforeIndex: must grow correctly past the centred capacity ----"
[
	| o |
	o := OrderedCollection new.
	1 to: 30 do: [:i | o add: i].
	Assert true: o size = 30.
	o add: 999 beforeIndex: 1.
	Assert true: o first = 999.
	Assert true: o size = 31.
	Assert true: (o at: 2) = 1.
]

"---- SortedCollection keeps order, even beyond the initial capacity ----"
[
	Assert true: (#(5 3 8 1 2) asSortedCollection) asArray = #(1 2 3 5 8).
	Assert true: ((30 to: 1 by: -1) asSortedCollection) asArray = (1 to: 30) asArray.
	Assert true: (#(1 5 2 4 3) asSortedCollection: [:a :b | a >= b]) asArray = #(5 4 3 2 1).
	Assert true: (SortedCollection new add: 3; add: 1; add: 2; yourself) first = 1.
]

"---- Object protocol: ifNil:/ifNotNil: and subclassResponsibility ----"
[
	Assert true: (nil ifNil: ['n'] ifNotNil: [:v | 'v']) = 'n'.
	Assert true: (42 ifNil: ['n'] ifNotNil: [:v | v * 2]) = 84.
	Assert true: (nil ifNil: [7]) = 7.
	Assert true: (5 ifNil: [7]) = 5.
	Assert do: [Object new subclassResponsibility] expect: SubClassResponsibility.
]

"---- Number math functions were stubs; now they coerce to Float ----"
[
	Assert true: 1 exp truncated = 2.
	Assert true: 100 ln truncated = 4.
	Assert true: (8 log: 2) rounded = 3.
	Assert true: 0 cos rounded = 1.
	Assert true: (2 raisedTo: 1/2) squared rounded = 2.
]
