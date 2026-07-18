[
	NumberRun := TestRun named: 'Number'.

	NumberRun check: 'readFrom decimal' equals: 1234567890 is: [ Number readFrom: (CollectionStream on: '1234567890') ].
	NumberRun check: 'readFrom base 16' equals: 16rFF is: [ Number readFrom: (CollectionStream on: 'FF') base: 16 ].
]

[ ^NumberRun report ]
