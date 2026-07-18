[
	| stream view |

	StreamViewRun := TestRun named: 'StreamView'.

	stream := CollectionStream on: 'abcdef'.
	view := StreamView on: stream limit: 3.

	StreamViewRun check: 'peek first' equals: $a is: [ view peek ].
	StreamViewRun check: 'next first' equals: $a is: [ view next ].
	StreamViewRun check: 'next: 2 within limit' equals: 'bc' is: [ view next: 2 ].
	StreamViewRun expect: 'next past limit raises' raises: Error in: [view next].
	StreamViewRun expect: 'next: past limit raises' raises: Error in: [view next: 2].
	StreamViewRun check: 'peek past limit is nil' equals: nil is: [ view peek ].

	stream := CollectionStream on: 'abcdef'.
	view := StreamView on: stream limit: 3.
	StreamViewRun check: 'contents limited' equals: 'abc' is: [ view contents ].

	stream := CollectionStream on: 'abcdef'.
	view := StreamView on: stream limit: 3.
	view position: 2.
	StreamViewRun check: 'position: 2 then next' equals: $b is: [ view next ].
	view position: 3.
	StreamViewRun expect: 'position: past limit raises' raises: Error in: [view position: 4].

	stream := CollectionStream on: 'abcdef'.
	view := StreamView on: stream limit: 3.
	view next: 2.
	view reset.
	StreamViewRun check: 'reset then next' equals: $a is: [ view next ].

	stream := CollectionStream on: 'abcdef'.
	view := StreamView on: stream limit: 3.
	view skip: 2.
	StreamViewRun check: 'skip: 2 then next' equals: $c is: [ view next ].
	view skip: -3.
	StreamViewRun check: 'skip: negative then next' equals: $a is: [ view next ].
]

[ ^StreamViewRun report ]
