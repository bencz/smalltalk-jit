"A small HTTP/1.1 client. Two ways to use it:

   one-shot:   HttpClient get: '/cards/1' host: addr port: 8080   -> HttpResponse

   keep-alive: client := HttpClient on: addr port: 8080.
               client get: '/cards/1'.
               client post: '/users' json: aDictionary.
               client close.

 The keep-alive form reuses one connection for many requests, which is what lets
 a load generator drive a very high request rate through a small pool of sockets."
HttpClient := Object [

	| socket conn host port |


	class on: anAddress port: aPort [
		^self new connectTo: anAddress port: aPort
	]


	class get: aPath host: anAddress port: aPort [
		| client response |
		client := self on: anAddress port: aPort.
		response := client get: aPath.
		client close.
		^response
	]


	connectTo: anAddress port: aPort [
		host := anAddress.
		port := aPort.
		socket := Socket connectTo: anAddress port: aPort.
		conn := HttpConnection on: socket.
		^self
	]


	"---- requests ----"

	get: aPath [ ^self send: 'GET' path: aPath body: nil ]
	delete: aPath [ ^self send: 'DELETE' path: aPath body: nil ]
	post: aPath json: anObject [ ^self send: 'POST' path: aPath body: (Json encode: anObject) ]
	put: aPath json: anObject [ ^self send: 'PUT' path: aPath body: (Json encode: anObject) ]


	send: aMethod path: aPath body: aBody [
		| out |
		out := CollectionStream with: (String new: 128).
		out nextPutAll: aMethod; nextPut: Character space; nextPutAll: aPath;
			nextPutAll: ' HTTP/1.1'.
		self endLineOn: out.
		out nextPutAll: 'Host: localhost'.
		self endLineOn: out.
		aBody notNil ifTrue: [
			out nextPutAll: 'Content-Type: application/json'.
			self endLineOn: out.
			out nextPutAll: 'Content-Length: '; nextPutAll: aBody size printString.
			self endLineOn: out ].
		self endLineOn: out.
		aBody notNil ifTrue: [ out nextPutAll: aBody ].
		socket nextPutAll: out contents.
		^HttpResponse readFrom: conn
	]


	endLineOn: aStream [
		aStream nextPut: Character cr.
		aStream nextPut: Character lf.
		^self
	]


	close [
		socket close.
		^self
	]

]
