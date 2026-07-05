"A parsed HTTP request. Built by HttpConnection>>readRequest. Exposes the method,
 path, HTTP version, header map (keys lowercased), parsed query string, decoded
 body, and route params bound by the router (e.g. the ':id' in '/cards/:id')."
HttpRequest := Object [

	| method path version headers query body params |


	class new [
		^super new initRequest
	]


	initRequest [
		"Only the header map is always needed. query/params stay nil until a
		 query string or a route param actually appears — most requests have
		 neither, so this saves two 64-slot Dictionary allocations each."
		headers := Dictionary new: 8.
		body := ''.
		^self
	]


	"---- building (used by the parser) ----"

	setMethod: aMethod target: aTarget version: aVersion [
		method := aMethod asUppercase.
		version := aVersion.
		self parseTarget: aTarget.
		^self
	]


	parseTarget: aTarget [
		| idx |
		idx := aTarget indexOf: $?.
		idx = 0
			ifTrue: [ path := aTarget ]
			ifFalse: [
				path := aTarget copyFrom: 1 to: idx - 1.
				self parseQuery: (aTarget copyFrom: idx + 1 to: aTarget size) ].
		^self
	]


	parseQuery: aString [
		query isNil ifTrue: [ query := Dictionary new: 8 ].
		(aString splitBy: $&) do: [:pair |
			| eq |
			eq := pair indexOf: $=.
			eq = 0
				ifTrue: [ query at: (self urlDecode: pair) put: '' ]
				ifFalse: [ query
					at: (self urlDecode: (pair copyFrom: 1 to: eq - 1))
					put: (self urlDecode: (pair copyFrom: eq + 1 to: pair size)) ] ].
		^self
	]


	urlDecode: aString [
		| out i size ch |
		out := CollectionStream with: (String new: aString size).
		i := 1.
		size := aString size.
		[ i <= size ] whileTrue: [
			ch := aString at: i.
			ch = $+
				ifTrue: [ out nextPut: Character space. i := i + 1 ]
				ifFalse: [ (ch = $% and: [ i + 2 <= size ])
					ifTrue: [
						out nextPut: (Character codePoint:
							((aString at: i + 1) asUppercase digitValue * 16)
								+ (aString at: i + 2) asUppercase digitValue).
						i := i + 3 ]
					ifFalse: [ out nextPut: ch. i := i + 1 ] ] ].
		^out contents
	]


	setHeader: aKey to: aValue [ headers at: aKey put: aValue. ^self ]
	setBody: aString [ body := aString. ^self ]


	"---- accessing ----"

	method [ ^method ]
	path [ ^path ]
	version [ ^version ]
	headers [ ^headers ]
	query [ ^query ]
	params [ ^params ]
	body [ ^body ]


	header: aKey [ ^headers at: aKey asLowercase ifAbsent: [ nil ] ]
	query: aKey [ query isNil ifTrue: [ ^nil ]. ^query at: aKey ifAbsent: [ nil ] ]
	param: aKey [ params isNil ifTrue: [ ^nil ]. ^params at: aKey ifAbsent: [ nil ] ]
	param: aKey put: aValue [
		params isNil ifTrue: [ params := Dictionary new: 8 ].
		params at: aKey put: aValue. ^self ]


	pathSegments [
		"'/cards/7' -> an OrderedCollection ('cards' '7'); empty for '/'"
		^path splitBy: $/
	]


	contentLength [
		| value |
		value := self header: 'content-length'.
		value isNil ifTrue: [ ^0 ].
		^value asNumber
	]


	jsonBody [
		(body isNil or: [ body isEmpty ]) ifTrue: [ ^nil ].
		^Json parse: body
	]


	isKeepAlive [
		| conn |
		conn := self header: 'connection'.
		version = 'HTTP/1.0' ifTrue: [
			^conn notNil and: [ conn asLowercase = 'keep-alive' ] ].
		"HTTP/1.1: keep-alive unless explicitly closed"
		conn isNil ifTrue: [ ^true ].
		^(conn asLowercase = 'close') not
	]


	printOn: aStream [
		aStream nextPutAll: method; nextPut: Character space; nextPutAll: path.
	]

]
