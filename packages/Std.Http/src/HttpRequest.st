"A parsed HTTP request. Built by HttpConnection>>readRequest. Exposes the method,
 path, HTTP version, header map (keys lowercased), parsed query string, decoded
 body, and route params bound by the router (e.g. the ':id' in '/cards/:id')."
HttpRequest := Object [

	| method path version headers rawHeaders query body params |


	class new [
		^super new initRequest
	]


	initRequest [
		"Nothing eager: the header map is built lazily from the raw header lines
		 only if the app actually reads a header (see #headers). The reader just
		 stashes raw 'Name: value' line refs via #addRawHeaderLine:, and
		 content-length / connection are found by an allocation-free raw scan
		 (#rawHeaderValueFor:). query/params also stay nil until a query string
		 or route param appears. So a plain keep-alive GET allocates no header
		 Dictionary, no per-header trim/lowercase/Association at all."
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


	setHeader: aKey to: aValue [ self headers at: aKey put: aValue. ^self ]
	setBody: aString [ body := aString. ^self ]


	"The reader stashes each raw 'Name: value' line as-is (no copy, no parse).
	 The header Dictionary is materialised lazily by #headers only if read."
	addRawHeaderLine: aLine [
		rawHeaders isNil ifTrue: [ rawHeaders := OrderedCollection new ].
		rawHeaders addLast: aLine.
		^self
	]


	parseHeaders [
		"Materialise the lowercased-key header Dictionary from the raw lines."
		headers := Dictionary new: 8.
		rawHeaders isNil ifTrue: [ ^headers ].
		rawHeaders do: [:line |
			| idx |
			idx := line indexOfByte: 58 startingAt: 1.   "$:"
			idx > 0 ifTrue: [
				headers
					at: (line copyFrom: 1 to: idx - 1) trimSeparators asLowercase
					put: (line copyFrom: idx + 1 to: line size) trimSeparators ] ].
		^headers
	]


	rawHeaderValueFor: aLowercaseName [
		"Find one header's trimmed value by scanning the raw lines, without
		 building the Dictionary. Allocates only the matched value string."
		rawHeaders isNil ifTrue: [ ^nil ].
		rawHeaders do: [:line |
			(line headerNameMatches: aLowercaseName) ifTrue: [
				| idx |
				idx := line indexOfByte: 58 startingAt: 1.
				^(line copyFrom: idx + 1 to: line size) trimSeparators ] ].
		^nil
	]


	"---- accessing ----"

	method [ ^method ]
	path [ ^path ]
	version [ ^version ]
	headers [ headers isNil ifTrue: [ self parseHeaders ]. ^headers ]
	query [ ^query ]
	params [ ^params ]
	body [ ^body ]


	header: aKey [ ^self headers at: aKey asLowercase ifAbsent: [ nil ] ]
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
		value := self rawHeaderValueFor: 'content-length'.
		value isNil ifTrue: [ ^0 ].
		^value asNumber
	]


	jsonBody [
		(body isNil or: [ body isEmpty ]) ifTrue: [ ^nil ].
		^Json parse: body
	]


	"Model binding: materialize the JSON body straight into an instance of
	 aClass (see Json decode:as: — keys map to instance variable names; an
	 optional class-side jsonTypes declares nested field classes)."
	jsonBodyAs: aClass [
		(body isNil or: [ body isEmpty ]) ifTrue: [ ^nil ].
		^Json decode: body as: aClass
	]


	isKeepAlive [
		| conn |
		conn := self rawHeaderValueFor: 'connection'.
		version = 'HTTP/1.0' ifTrue: [
			^conn notNil and: [ conn sameAsciiCaseAs: 'keep-alive' ] ].
		"HTTP/1.1: keep-alive unless explicitly closed"
		conn isNil ifTrue: [ ^true ].
		^(conn sameAsciiCaseAs: 'close') not
	]


	printOn: aStream [
		aStream nextPutAll: method; nextPut: Character space; nextPutAll: path.
	]

]
