"An HTTP response. Handlers build one fluently:

     ^(HttpResponse ok) json: aDictionary
     ^HttpResponse created: aDictionary
     ^(HttpResponse status: 418) text: 'short and stout'

 The server serialises it with writeOn:keepAlive:. HttpResponse also parses
 responses on the client side via readFrom:."
HttpResponse := Object [

	| status reason headers body |


	class new [
		^super new initResponse
	]


	initResponse [
		status := 200.
		reason := 'OK'.
		headers := Dictionary new.
		body := ''.
		^self
	]


	"---- instance creation ----"

	class status: aCode [ ^self new status: aCode ]
	class ok [ ^self new status: 200 ]
	class created: anObject [ ^(self new status: 201) json: anObject ]
	class noContent [ ^self new status: 204 ]
	class notFound [ ^(self new status: 404) json: (self errorBody: 'not found') ]
	class badRequest: aString [ ^(self new status: 400) json: (self errorBody: aString) ]
	class methodNotAllowed [ ^(self new status: 405) json: (self errorBody: 'method not allowed') ]
	class serverError: aString [ ^(self new status: 500) json: (self errorBody: aString) ]


	class errorBody: aString [
		| d |
		d := Dictionary new.
		d at: 'error' put: aString.
		^d
	]


	class reasonFor: aCode [
		aCode = 200 ifTrue: [ ^'OK' ].
		aCode = 201 ifTrue: [ ^'Created' ].
		aCode = 204 ifTrue: [ ^'No Content' ].
		aCode = 400 ifTrue: [ ^'Bad Request' ].
		aCode = 404 ifTrue: [ ^'Not Found' ].
		aCode = 405 ifTrue: [ ^'Method Not Allowed' ].
		aCode = 500 ifTrue: [ ^'Internal Server Error' ].
		^'OK'
	]


	"---- fluent building ----"

	status: aCode [
		status := aCode.
		reason := self class reasonFor: aCode.
		^self
	]


	setStatus: aCode [ status := aCode. ^self ]

	header: aKey put: aValue [ headers at: aKey put: aValue. ^self ]
	body: aString [ body := aString. ^self ]


	text: aString [
		body := aString.
		headers at: 'Content-Type' put: 'text/plain; charset=utf-8'.
		^self
	]


	json: anObject [
		body := Json encode: anObject.
		headers at: 'Content-Type' put: 'application/json'.
		^self
	]


	"---- accessing ----"

	status [ ^status ]
	reason [ ^reason ]
	headers [ ^headers ]
	body [ ^body ]


	header: aKey [ ^headers at: aKey asLowercase ifAbsent: [ nil ] ]


	jsonBody [
		(body isNil or: [ body isEmpty ]) ifTrue: [ ^nil ].
		^Json parse: body
	]


	"---- writing (server side) ----"

	writeOn: aSocket keepAlive: keepAlive [
		| out |
		out := CollectionStream with: (String new: 256).
		out nextPutAll: 'HTTP/1.1 '.
		out nextPutAll: status printString.
		out nextPut: Character space.
		out nextPutAll: reason.
		self endLineOn: out.
		headers keysAndValuesDo: [:k :v |
			| lk |
			lk := k asLowercase.
			(lk = 'content-length' or: [ lk = 'connection' ])
				ifFalse: [ self putHeader: k value: v on: out ] ].
		self putHeader: 'Content-Length' value: body size on: out.
		self putHeader: 'Connection'
			value: (keepAlive ifTrue: [ 'keep-alive' ] ifFalse: [ 'close' ])
			on: out.
		self endLineOn: out.
		out nextPutAll: body.
		aSocket nextPutAll: out contents.
		^self
	]


	putHeader: aKey value: aValue on: aStream [
		aStream nextPutAll: aKey; nextPutAll: ': '.
		aStream nextPutAll: ((aValue isKindOf: String) ifTrue: [ aValue ] ifFalse: [ aValue printString ]).
		self endLineOn: aStream.
		^self
	]


	endLineOn: aStream [
		aStream nextPut: Character cr.
		aStream nextPut: Character lf.
		^self
	]


	"---- reading (client side) ----"

	class readFrom: aConnection [
		| response line parts length |
		response := self new.
		line := aConnection readLine.
		line isNil ifTrue: [ ^nil ].
		parts := line splitBy: Character space.
		parts size >= 2 ifTrue: [ response setStatus: (parts at: 2) asNumber ].
		[ line := aConnection readLine.
		  line isNil or: [ line isEmpty ] ] whileFalse: [
			| idx |
			idx := line indexOf: $:.
			idx > 0 ifTrue: [
				response header: (line copyFrom: 1 to: idx - 1) trimSeparators asLowercase
					put: (line copyFrom: idx + 1 to: line size) trimSeparators ] ].
		length := response contentLengthValue.
		length > 0 ifTrue: [ response body: (aConnection readBytes: length) ].
		^response
	]


	contentLengthValue [
		| value |
		value := headers at: 'content-length' ifAbsent: [ nil ].
		value isNil ifTrue: [ ^0 ].
		^value asNumber
	]


	printOn: aStream [
		aStream nextPutAll: 'HTTP '; nextPutAll: status printString;
			nextPut: Character space; nextPutAll: reason.
	]

]
