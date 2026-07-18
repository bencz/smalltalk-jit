[
	CharRun := TestRun named: 'Character'.

	CharRun check: 'space isSeparator' that: [Character space isSeparator].
]


[ ^CharRun report ]
