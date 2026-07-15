"A buffered, incremental reader over a Socket. It reads raw bytes with the
 non-blocking socket primitive (which parks the fiber until data arrives, and
 returns 0 when the peer closes), buffering unconsumed bytes so a message that
 spans several reads is handled correctly and any bytes belonging to the next
 keep-alive request are preserved. Used by HttpServer to read requests and by
 HttpClient (via HttpResponse readFrom:) to read responses."
HttpConnection := Object [

	| socket pending pos chunk |


	class on: aSocket [
		^self new setSocket: aSocket
	]


	setSocket: aSocket [
		socket := aSocket.
		pending := String new: 0.
		"pos = 1-based index of the next unconsumed byte in `pending`. Parsing
		 advances the cursor instead of re-slicing the buffer per line."
		pos := 1.
		"one reusable read buffer per connection, so the hot path allocates
		 nothing large per request"
		chunk := String new: 4096.
		^self
	]


	socket [ ^socket ]


	"---- low-level buffered reads ----"

	readMore [
		"Drop the already-consumed prefix, then pull one chunk from the socket.
		 Answers the number of bytes read; 0 means the peer closed."
		| n |
		pos > 1 ifTrue: [
			pending := pending copyFrom: pos to: pending size.
			pos := 1 ].
		n := socket directNext: 4096 into: chunk startingAt: 1.
		n > 0 ifTrue: [ pending := pending , (chunk copyFrom: 1 to: n) ].
		^n
	]


	readLine [
		"Answer the next CRLF- (or LF-) terminated line without its terminator,
		 or nil if the peer closes before a full line arrives. Scans in place
		 from the cursor with a memchr-backed indexOfByte:."
		| idx stop |
		[
			idx := pending indexOfByte: 10 startingAt: pos.   "LF"
			idx > 0 ifTrue: [
				stop := idx - 1.
				(stop >= pos and: [ (pending at: stop) codePoint = 13 ])
					ifTrue: [ stop := stop - 1 ].              "drop trailing CR"
				^self consumeFrom: pos to: stop advancingTo: idx + 1 ].
			self readMore = 0 ifTrue: [ ^nil ]
		] repeat.
	]


	consumeFrom: start to: stop advancingTo: nextPos [
		| line |
		line := pending copyFrom: start to: stop.
		pos := nextPos.
		^line
	]


	readBytes: n [
		"Answer exactly n bytes (fewer only if the peer closes early)."
		| result |
		[ (pending size - pos + 1) < n ] whileTrue: [
			self readMore = 0 ifTrue: [
				result := pending copyFrom: pos to: pending size.
				pending := String new: 0.
				pos := 1.
				^result ] ].
		result := pending copyFrom: pos to: pos + n - 1.
		pos := pos + n.
		^result
	]


	"---- request parsing (server side) ----"

	readRequest [
		"Answer the next HttpRequest, or nil when the connection is done."
		| line parts request |
		line := self readLine.
		line isNil ifTrue: [ ^nil ].
		line isEmpty ifTrue: [ ^self readRequest ].   "tolerate stray blank lines"
		parts := line splitBy: Character space.
		parts size < 2 ifTrue: [ ^nil ].              "malformed request line"
		request := HttpRequest new.
		request
			setMethod: (parts at: 1)
			target: (parts at: 2)
			version: (parts size >= 3 ifTrue: [ parts at: 3 ] ifFalse: [ 'HTTP/1.0' ]).
		self readHeadersInto: request.
		self readBodyInto: request.
		^request
	]


	readHeadersInto: request [
		| line idx |
		[
			line := self readLine.
			(line isNil or: [ line isEmpty ]) ifTrue: [ ^self ].
			idx := line indexOfByte: 58 startingAt: 1.   "$:"
			idx > 0 ifTrue: [
				request
					setHeader: (line copyFrom: 1 to: idx - 1) trimSeparators asLowercase
					to: (line copyFrom: idx + 1 to: line size) trimSeparators ]
		] repeat.
	]


	readBodyInto: request [
		| length |
		length := request contentLength.
		length > 0 ifTrue: [ request setBody: (self readBytes: length) ].
		^self
	]

]
