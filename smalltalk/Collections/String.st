String := ArrayedCollection [

	<shape: StringShape>


	"comparing"

	hash [
		<primitive: StringHashPrimitive>
	]


	= aCollection [
		"memcmp fast path; falls back to the element-by-element compare for
		 non-String operands."
		<primitive: ArrayEqualsPrimitive>
		^super = aCollection
	]


	sameAsciiCaseAs: aString [
		"ASCII case-insensitive equality WITHOUT allocating (unlike
		 `self asLowercase = aString`). For the HTTP hot path, where header
		 names/tokens are compared but never need a lowercased copy."
		<primitive: StringAsciiCaseEqualsPrimitive>
		| n a b la |
		n := self size.
		n = aString size ifFalse: [ ^false ].
		1 to: n do: [ :i |
			a := (self at: i) codePoint.
			b := (aString at: i) codePoint.
			a = b ifFalse: [
				la := a bitOr: 32.
				((la between: 97 and: 122) and: [ la = (b bitOr: 32) ])
					ifFalse: [ ^false ] ] ].
		^true
	]


	startsWithAsciiCase: aPrefix [
		"True if the receiver begins with aPrefix, ASCII case-insensitive, without
		 allocating. C fast path; the .st fallback covers non-byte operands."
		<primitive: StringStartsWithAsciiCasePrimitive>
		| n a b la |
		n := aPrefix size.
		n > self size ifTrue: [ ^false ].
		1 to: n do: [ :i |
			a := (self at: i) codePoint.
			b := (aPrefix at: i) codePoint.
			a = b ifFalse: [
				la := a bitOr: 32.
				((la between: 97 and: 122) and: [ la = (b bitOr: 32) ]) ifFalse: [ ^false ] ] ].
		^true
	]


	headerNameMatches: aLowercaseName [
		"True if the name part of this 'Name: value' header line (the bytes before
		 the first ':', minus any separators before it) equals aLowercaseName,
		 ASCII case-insensitive. Allocation-free, so the HTTP reader finds
		 content-length / connection without materialising the header dict. HTTP
		 header names start at column 0, so anchor with startsWithAsciiCase: (in C)."
		| i sz |
		(self startsWithAsciiCase: aLowercaseName) ifFalse: [ ^false ].
		sz := self size.
		i := aLowercaseName size + 1.
		[ i <= sz and: [ (self at: i) isSeparator ] ] whileTrue: [ i := i + 1 ].
		^i <= sz and: [ (self at: i) codePoint = 58 ]
	]


	"scanning"

	indexOfByte: anInteger startingAt: start [
		"memchr fast path for a raw byte value (0..255); answers a 1-based index
		 or 0. Used by the HTTP parser to find line terminators quickly."
		<primitive: IndexOfBytePrimitive>
		start to: self size do: [ :i | (self at: i) codePoint = anInteger ifTrue: [ ^i ] ].
		^0
	]


	"converting"

	asSymbol [
		<primitive: StringAsSymbolPrimitive>
	]


	asLowercase [
		"C fast path: pure ASCII fold (A-Z -> a-z). The Smalltalk fallback runs
		 for non-String byte shapes (e.g. Symbol) so the receiver species is kept."
		<primitive: StringAsciiLowercasePrimitive>
		| newString |
		newString := self class new: self size.
		1 to: self size do: [ :i | newString at: i put: (self at: i) asLowercase].
		^newString
	]


	asUppercase [
		<primitive: StringAsciiUppercasePrimitive>
		| newString |
		newString := self class new: self size.
		1 to: self size do: [ :i | newString at: i put: (self at: i) asUppercase].
		^newString
	]


	asNumber [
		"C fast path for a plain base-10 SmallInteger; falls back to the full
		 stream parser for signs/floats/exponents/large values/trailing text."
		<primitive: StringToIntegerPrimitive>
		^Number readFrom: (CollectionStream on: self)
	]


	trimSeparators [
		"C fast path: trim {tab,lf,ff,cr,space} from both ends in one pass."
		<primitive: StringTrimSeparatorsPrimitive>
		1 to: self size do: [ :start |
			(self at: start) isSeparator ifFalse: [
				self size to: start by: -1 do: [ :stop |
					(self at: stop) isSeparator ifFalse: [
						^self copyFrom: start to: stop
					].
				].
			].
		].
		^''
	]


	"primitives"

	isKeyword [
		^self size > 1 and: [self last = $:]
	]


	keywords [
		| keywords pos |

		keywords := OrderedCollection new.
		pos := 1.

		self keysAndValuesDo: [ :i :ch |
			ch = $: ifTrue: [
				keywords add: (self copyFrom: pos to: i).
				pos := i + 1].
		].
		keywords isEmpty ifTrue: [keywords add: self].
		^keywords
	]


	"printing"

	printOn: aStream [
		aStream
			nextPut: $';
			nextPutAll: self;
			nextPut: $'.
	]

]
