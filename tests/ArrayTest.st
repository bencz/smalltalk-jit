[
	| array |

	ArrayRun := TestRun named: 'Array'.

	array := Array new: 2.

	ArrayRun check: 'new size' equals: 2 is: [ array size ].
	ArrayRun check: 'slot 1 starts nil' that: [ (array at: 1) isNil ].
	ArrayRun check: 'slot 2 starts nil' that: [ (array at: 2) isNil ].
	ArrayRun check: 'at:put: returns value' equals: 3 is: [ array at: 1 put: 3 ].
]

[
	ArrayRun check: 'equal arrays' that: [ (Array with: 1 with: 2) = (Array with: 1 with: 2) ].
	ArrayRun check: 'unequal arrays' that: [ (Array with: 1 with: 2) ~= (Array with: 1 with: 3) ].

	ArrayRun check: 'equal hashes' that: [ (Array with: 1 with: 2) hash = (Array with: 1 with: 2) hash ].
	ArrayRun check: 'unequal hashes' that: [ (Array with: 1 with: 2) hash ~= (Array with: 1 with: 3) hash ].
]

[ ^ArrayRun report ]
