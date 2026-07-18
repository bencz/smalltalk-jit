[
	SmallIntegerRun := TestRun named: 'SmallInteger'.

	SmallIntegerRun expect: 'lt non-number' raises: Error in: [6 < Object].
	SmallIntegerRun expect: 'add non-number' raises: Error in: [6 + Object].
	SmallIntegerRun expect: 'sub non-number' raises: Error in: [6 - Object].
	SmallIntegerRun expect: 'mul non-number' raises: Error in: [6 * Object].
	SmallIntegerRun expect: 'idiv non-number' raises: Error in: [6 // Object].
	SmallIntegerRun expect: 'mod non-number' raises: Error in: [6 \\ Object].
	SmallIntegerRun expect: 'div non-number' raises: Error in: [6 / Object].
	SmallIntegerRun expect: 'quo non-number' raises: Error in: [6 quo: Object].
	SmallIntegerRun expect: 'rem non-number' raises: Error in: [6 rem: Object].
	SmallIntegerRun expect: 'bitAnd non-number' raises: Error in: [6 bitAnd: Object].
	SmallIntegerRun expect: 'bitOr non-number' raises: Error in: [6 bitOr: Object].
	SmallIntegerRun expect: 'bitXor non-number' raises: Error in: [6 bitXor: Object].
	SmallIntegerRun expect: 'bitShift non-number' raises: Error in: [6 bitShift: Object].
]

[ ^SmallIntegerRun report ]
