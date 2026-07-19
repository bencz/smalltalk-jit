[ | t |
	t := TestRun named: 'Hello'.
	t check: 'truth holds' that: [ true ].
	^t report
]
