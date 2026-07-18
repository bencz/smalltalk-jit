[
	StringRun := TestRun named: 'String'.

	StringRun check: 'splitBy: space' equals: (OrderedCollection with: 'abc' with: 'gh' with: 'i')
		is: [ '  abc gh i  ' splitBy: Character space ].
	StringRun check: 'splitByAll: digits' equals: (OrderedCollection with: 'abc' with: 'gh' with: 'i')
		is: [ '2abc1gh3i1' splitByAll: #($1 $2 $3) ].
]

[ ^StringRun report ]
