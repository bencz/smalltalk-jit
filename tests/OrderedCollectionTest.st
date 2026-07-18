[
	| collection expected |

	OrderedCollectionRun := TestRun named: 'OrderedCollection'.

	collection := OrderedCollection new.
	collection add: 1; add: 2; add: 3.
	OrderedCollectionRun check: 'removeIndex: middle returns element' equals: 2 is: [ collection removeIndex: 2 ].
	OrderedCollectionRun check: 'removeIndex: middle keeps rest' equals: (OrderedCollection new add: 1; add: 3; yourself) is: [ collection ].

	collection := OrderedCollection new.
	collection add: 1; add: 2; add: 3.
	OrderedCollectionRun check: 'removeIndex: first returns element' equals: 1 is: [ collection removeIndex: 1 ].
	OrderedCollectionRun check: 'removeIndex: first keeps rest' equals: (OrderedCollection new add: 2; add: 3; yourself) is: [ collection ].

	collection := OrderedCollection new.
	collection add: 1; add: 2; add: 3.
	OrderedCollectionRun check: 'removeIndex: last returns element' equals: 3 is: [ collection removeIndex: 3 ].
	OrderedCollectionRun check: 'removeIndex: last keeps rest' equals: (OrderedCollection new add: 1; add: 2; yourself) is: [ collection ].

	collection := OrderedCollection new.
	collection add: 1; add: 2; add: 3; add: 4; add: 5; add: 6.

	expected := OrderedCollection new.
	expected add: 1; add: 2; add: 7; add: 3; add: 4; add: 5; add: 6.
	collection add: 7 beforeIndex: 3.
	OrderedCollectionRun check: 'add:beforeIndex: inserts' equals: expected is: [ collection ].

	expected := OrderedCollection new.
	expected add: 1; add: 2; add: 7; add: 3; add: 8; add: 4; add: 5; add: 6.
	collection add: 8 afterIndex: 4.
	OrderedCollectionRun check: 'add:afterIndex: inserts' equals: expected is: [ collection ].

	OrderedCollectionRun expect: 'last on empty raises' raises: Error in: [ OrderedCollection new last ].
]

[
	OrderedCollectionRun check: 'asArray' equals: #(1 2 3) is: [ (OrderedCollection new add: 1; add: 2; add: 3; yourself) asArray ].
]

[ ^OrderedCollectionRun report ]
