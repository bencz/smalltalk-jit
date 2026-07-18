[
	NilRun := TestRun named: 'UndefinedObject'.

	NilRun check: 'nil isNil' that: [nil isNil].
	NilRun check: 'nil copy is nil' that: [nil == nil copy].
]


[ ^NilRun report ]
