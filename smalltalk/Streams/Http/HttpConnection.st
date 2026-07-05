"A buffered, incremental reader over a Socket. It reads raw bytes with the
 non-blocking socket primitive (which parks the fiber until data arrives, and
 returns 0 when the peer closes), buffering unconsumed bytes so a message that
 spans several reads is handled correctly and any bytes belonging to the next
 keep-alive request are preserved. Used by HttpServer to read requests and by
 HttpClient (via HttpResponse readFrom:) to read responses."
HttpConnection := Object [

	| socket pending chunk |


	class on: aSocket [
		^self new setSocket: aSocket
	]


	setSocket: aSocket [
		socket := aSocket.
		pending := String new: 0.
		"one reusable read buffer per connection, so the hot path allocates
		 nothing large per request"
		chunk := String new: 4096.
		^self
	]


	socket [ ^socket ]


	"---- low-level buffered reads ----"

	readMore [
		"Pull one chunk from the socket into the pending buffer. Answers the
		 number of bytes read; 0 means the peer closed the connection."
		| n |
		n := socket directNext: 4096 into: chunk startingAt: 1.
		n > 0 ifTrue: [ pending := pending , (chunk copyFrom: 1 to: n) ].
		^n
	]


	readLine [
		"Answer the next CRLF- (or LF-) terminated line without its terminator,
		 or nil if the peer closes before a full line arrives."
		| idx line |
		[
			idx := pending indexOf: Character lf.
			idx > 0 ifTrue: [
				line := pending copyFrom: 1 to: idx - 1.
				pending := pending copyFrom: idx + 1 to: pending size.
				(line size > 0 and: [ (line at: line size) = Character cr ])
					ifTrue: [ line := line copyFrom: 1 to: line size - 1 ].
				^line ].
			self readMore = 0 ifTrue: [ ^nil ]
		] repeat.
	]


	readBytes: n [
		"Answer exactly n bytes (fewer only if the peer closes early)."
		| result |
		[ pending size < n ] whileTrue: [
			self readMore = 0 ifTrue: [
				result := pending.
				pending := String new: 0.
				^result ] ].
		result := pending copyFrom: 1 to: n.
		pending := pending copyFrom: n + 1 to: pending size.
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
			idx := line indexOf: $:.
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
