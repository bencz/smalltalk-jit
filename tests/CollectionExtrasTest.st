"Regression tests for the collection/number protocol added and
 fixed alongside the samples: enumeration helpers, conversions,
 reverse, concatenation, copyFrom:, the insert:beforeIndex: growth
 fix, SortedCollection, and the Number math functions."

"---- enumeration on non-OrderedCollections ----"
[
	CollectionExtrasRun := TestRun named: 'CollectionExtras'.

	CollectionExtrasRun check: 'interval select even' equals: #(2 4 6 8 10) is: [ ((1 to: 10) select: [:x | x even]) asArray ].
	CollectionExtrasRun check: 'array reject greater than 4' equals: #(3 1) is: [ (#(5 3 8 1 9) reject: [:x | x > 4]) asArray ].
	CollectionExtrasRun check: 'set collect size' equals: 3 is: [ ((Set new add: 1; add: 2; add: 3; yourself) collect: [:x | x]) size ].
	CollectionExtrasRun check: 'interval count even' equals: 5 is: [ (1 to: 10) count: [:x | x even] ].
	CollectionExtrasRun check: 'detect:ifNone: finds even' equals: 8 is: [ #(3 5 8 9) detect: [:x | x even] ifNone: [0] ].
]

"---- reducing and converting ----"
[
	CollectionExtrasRun check: 'sum' equals: 15 is: [ #(1 2 3 4 5) sum ].
	CollectionExtrasRun check: 'max' equals: 9 is: [ #(3 1 4 1 5 9 2 6) max ].
	CollectionExtrasRun check: 'min' equals: 1 is: [ #(3 1 4 1 5 9 2 6) min ].
	CollectionExtrasRun check: 'average' equals: 4 is: [ #(2 4 6) average ].
	CollectionExtrasRun check: 'asOrderedCollection' equals: (OrderedCollection with: 5 with: 3 with: 8) is: [ #(5 3 8) asOrderedCollection ].
	CollectionExtrasRun check: 'interval asArray' equals: #(1 2 3 4) is: [ (1 to: 4) asArray ].
]

"---- reverse and with:collect: ----"
[
	CollectionExtrasRun check: 'array reverse' equals: #(3 2 1) is: [ #(1 2 3) reverse ].
	CollectionExtrasRun check: 'interval reverse' equals: #(5 4 3 2 1) is: [ (1 to: 5) reverse ].
	CollectionExtrasRun check: 'oc reverse asArray' equals: #(3 2 1) is: [ (OrderedCollection withAll: #(1 2 3)) reverse asArray ].
	CollectionExtrasRun check: 'with:collect: sums pairs' equals: #(11 22 33) is: [ #(1 2 3) with: #(10 20 30) collect: [:a :b | a + b] ].
	CollectionExtrasRun check: 'asUppercase' equals: 'HELLO' is: [ 'hello' asUppercase ].
]

"---- do:separatedBy: builds a nicely formatted string ----"
[
	| s |
	s := CollectionStream with: (String new: 0).
	#(1 2 3) do: [:x | s nextPutAll: x printString] separatedBy: [s nextPutAll: ', '].
	CollectionExtrasRun check: 'do:separatedBy: contents' equals: '1, 2, 3' is: [ s contents ].
]

"---- concatenation and copyFrom: on growable / interval ----"
[
	CollectionExtrasRun check: 'oc concatenation' equals: #(1 2 3 4) is: [ ((OrderedCollection withAll: #(1 2)) , (OrderedCollection withAll: #(3 4))) asArray ].
	CollectionExtrasRun check: 'interval concatenation' equals: #(1 2 3 4 5) is: [ (1 to: 3) , (4 to: 5) ].
	CollectionExtrasRun check: 'oc copyFrom:to:' equals: #(20 30) is: [ ((OrderedCollection withAll: #(10 20 30 40)) copyFrom: 2 to: 3) asArray ].
	CollectionExtrasRun check: 'interval copyFrom:to:' equals: #(3 4 5) is: [ (1 to: 10) copyFrom: 3 to: 5 ].
]

"---- insert:beforeIndex: must grow correctly past the centred capacity ----"
[
	| o |
	o := OrderedCollection new.
	1 to: 30 do: [:i | o add: i].
	CollectionExtrasRun check: 'size before insert' equals: 30 is: [ o size ].
	o add: 999 beforeIndex: 1.
	CollectionExtrasRun check: 'insert beforeIndex 1 becomes first' equals: 999 is: [ o first ].
	CollectionExtrasRun check: 'size after insert' equals: 31 is: [ o size ].
	CollectionExtrasRun check: 'old first shifted to 2' equals: 1 is: [ o at: 2 ].
]

"---- SortedCollection keeps order, even beyond the initial capacity ----"
[
	CollectionExtrasRun check: 'asSortedCollection sorts' equals: #(1 2 3 5 8) is: [ (#(5 3 8 1 2) asSortedCollection) asArray ].
	CollectionExtrasRun check: 'sorted beyond initial capacity' equals: (1 to: 30) asArray is: [ ((30 to: 1 by: -1) asSortedCollection) asArray ].
	CollectionExtrasRun check: 'asSortedCollection: descending block' equals: #(5 4 3 2 1) is: [ (#(1 5 2 4 3) asSortedCollection: [:a :b | a >= b]) asArray ].
	CollectionExtrasRun check: 'sorted first after adds' equals: 1 is: [ (SortedCollection new add: 3; add: 1; add: 2; yourself) first ].
]

"---- Object protocol: ifNil:/ifNotNil: and subclassResponsibility ----"
[
	CollectionExtrasRun check: 'nil ifNil:ifNotNil:' equals: 'n' is: [ nil ifNil: ['n'] ifNotNil: [:v | 'v'] ].
	CollectionExtrasRun check: 'value ifNil:ifNotNil:' equals: 84 is: [ 42 ifNil: ['n'] ifNotNil: [:v | v * 2] ].
	CollectionExtrasRun check: 'nil ifNil:' equals: 7 is: [ nil ifNil: [7] ].
	CollectionExtrasRun check: 'value ifNil:' equals: 5 is: [ 5 ifNil: [7] ].
	CollectionExtrasRun expect: 'subclassResponsibility raises' raises: SubClassResponsibility in: [Object new subclassResponsibility].
]

"---- Number math functions were stubs; now they coerce to Float ----"
[
	CollectionExtrasRun check: 'exp truncated' equals: 2 is: [ 1 exp truncated ].
	CollectionExtrasRun check: 'ln truncated' equals: 4 is: [ 100 ln truncated ].
	CollectionExtrasRun check: 'log: rounded' equals: 3 is: [ (8 log: 2) rounded ].
	CollectionExtrasRun check: 'cos rounded' equals: 1 is: [ 0 cos rounded ].
	CollectionExtrasRun check: 'raisedTo: half squared' equals: 2 is: [ (2 raisedTo: 1/2) squared rounded ].
]

[ ^CollectionExtrasRun report ]
