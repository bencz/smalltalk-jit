"StreamEnumerationTest: Stream>>collect:/select:/reject: materialise the
 remaining stream into an OrderedCollection via do:, over read streams of both
 an Array and a String. Also exercises SequenceableCollection>>readStream /
 writeStream and Collection class>>streamContents: for the arrayed species."

[
	| t ws |
	t := TestRun named: 'StreamEnumeration'.

	"---- collect: / select: / reject: over an Array read stream ----"
	t check: 'stream collect: over Array' equals: #(10 20 30 40) is: [
		(#(1 2 3 4) readStream collect: [:x | x * 10]) asArray ].
	t check: 'stream select: over Array' equals: #(1 3 5) is: [
		(#(1 2 3 4 5) readStream select: [:x | x odd]) asArray ].
	t check: 'stream reject: over Array' equals: #(2 4) is: [
		(#(1 2 3 4 5) readStream reject: [:x | x odd]) asArray ].
	t check: 'stream collect: answers an OrderedCollection' that: [
		(#(1 2 3) readStream collect: [:x | x]) isKindOf: OrderedCollection ].

	"---- collect: / select: / reject: over a String read stream ----"
	t check: 'stream collect: over String' equals: (Array with: $A with: $B with: $C) is: [
		('abc' readStream collect: [:c | c asUppercase]) asArray ].
	t check: 'stream select: over String' equals: 2 is: [
		('hello' readStream select: [:c | c = $l]) size ].
	t check: 'stream reject: over String' equals: 3 is: [
		('hello' readStream reject: [:c | c = $l]) size ].

	"---- readStream basics ----"
	t check: 'readStream reads in order' that: [
		| s | s := #(7 8 9) readStream.
		(s next = 7) and: [ (s next = 8) and: [ s next = 9 ] ] ].

	"---- writeStream over a fresh buffer of the same species ----"
	ws := #(1 2 3) writeStream.
	ws nextPut: 9; nextPut: 8.
	t check: 'Array writeStream contents' equals: #(9 8) is: [ ws contents ].
	ws := 'x' writeStream.
	ws nextPutAll: 'hi'; nextPut: $!.
	t check: 'String writeStream contents' equals: 'hi!' is: [ ws contents ].

	"---- streamContents: on the class side ----"
	t check: 'String streamContents:' equals: 'abc' is: [
		String streamContents: [:s | s nextPutAll: 'ab'; nextPut: $c] ].
	t check: 'Array streamContents:' equals: #(1 2 3) is: [
		Array streamContents: [:s | s nextPut: 1; nextPut: 2; nextPut: 3] ].
	t check: 'streamContents: grows past the initial buffer' equals: 100 is: [
		(String streamContents: [:s | 100 timesRepeat: [s nextPut: $x]]) size ].

	^t report
]
