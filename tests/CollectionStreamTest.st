[
	| stream |

	CollectionStreamRun := TestRun named: 'CollectionStream'.

	stream := CollectionStream on: 'abbccdd'.

	CollectionStreamRun check: 'peek first' equals: $a is: [ stream peek ].
	CollectionStreamRun check: 'peek does not advance' equals: $a is: [ stream peek ].

	CollectionStreamRun check: 'next first' equals: $a is: [ stream next ].
	CollectionStreamRun check: 'next: 2 reads bb' equals: 'bb' is: [ stream next: 2 ].
	CollectionStreamRun check: 'next: 2 reads cc' equals: 'cc' is: [ stream next: 2 ].
	CollectionStreamRun check: 'next reads d' equals: $d is: [ stream next ].
	CollectionStreamRun expect: 'next: past end raises' raises: OutOfRangeError in: [stream next: 2].

	stream := CollectionStream on: 'foo' copy.
	CollectionStreamRun check: 'contents at start' equals: 'foo' copy is: [ stream contents ].
	CollectionStreamRun check: 'upToEnd at start' equals: 'foo' copy is: [ stream upToEnd ].

	stream next.
	CollectionStreamRun check: 'contents after next' equals: 'foo' copy is: [ stream contents ].
	CollectionStreamRun check: 'upToEnd after next' equals: 'oo' copy is: [ stream upToEnd ].

	stream := CollectionStream with: (String new: 1).
	stream nextPut: $a; nextPut: $a.
	stream nextPutAll: 'bb'.
	CollectionStreamRun check: 'write stream grows' equals: 'aabb' copy is: [ stream contents ].

	stream := CollectionStream on: 'abc' copy.
	stream next; nextPut: $x.
	CollectionStreamRun check: 'nextPut: overwrites middle' equals: 'axc' copy is: [ stream contents ].
	stream nextPutAll: 'xx'.
	CollectionStreamRun check: 'nextPutAll: extends' equals: 'axxx' copy is: [ stream contents ].

	stream := CollectionStream with: 'abc' copy.
	CollectionStreamRun check: 'with: starts atEnd' that: [ stream atEnd ].

	stream := CollectionStream on: 'abc' copy.
	CollectionStreamRun check: 'on: not atEnd' that: [ stream atEnd = false ].
	stream next: 2.
	CollectionStreamRun check: 'not atEnd after next: 2' that: [ stream atEnd = false ].
	stream next.
	CollectionStreamRun check: 'atEnd after reading all' that: [ stream atEnd ].
]

[ ^CollectionStreamRun report ]
