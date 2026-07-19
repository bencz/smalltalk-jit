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


	"ordering (lexicographic by byte; String is not a Magnitude, so the whole
	 comparison protocol is defined here rather than inherited)"

	compare: aString [
		"Answer a negative integer, zero, or a positive integer as the receiver is
		 lexicographically less than, equal to, or greater than aString. Bytes are
		 compared by code point; a proper prefix sorts before the longer sequence."
		| n an bn a b |
		an := self size.
		bn := aString size.
		n := an min: bn.
		1 to: n do: [ :i |
			a := (self at: i) codePoint.
			b := (aString at: i) codePoint.
			a < b ifTrue: [^-1].
			a > b ifTrue: [^1] ].
		an < bn ifTrue: [^-1].
		an > bn ifTrue: [^1].
		^0
	]


	< aString [
		^(self compare: aString) < 0
	]


	<= aString [
		^(self compare: aString) <= 0
	]


	> aString [
		^(self compare: aString) > 0
	]


	>= aString [
		^(self compare: aString) >= 0
	]


	sameAs: aString [
		"Case-insensitive equality (ASCII fold). Uses the allocation-free ASCII
		 fast path; its own fallback handles non-String byte operands."
		^self sameAsciiCaseAs: aString
	]


	"scanning"

	indexOfByte: anInteger startingAt: start [
		"memchr fast path for a raw byte value (0..255); answers a 1-based index
		 or 0. Used by the HTTP parser to find line terminators quickly."
		<primitive: IndexOfBytePrimitive>
		start to: self size do: [ :i | (self at: i) codePoint = anInteger ifTrue: [ ^i ] ].
		^0
	]


	includesSubstring: aString [
		"True if aString occurs anywhere in the receiver; the empty string is
		 considered present."
		aString isEmpty ifTrue: [^true].
		^(self indexOfSubCollection: aString startingAt: 1) > 0
	]


	indexOf: aCharacter startingAt: start [
		"Convenience over indexOf:startingAt:ifAbsent:, answering 0 when absent."
		^self indexOf: aCharacter startingAt: start ifAbsent: [0]
	]


	occurrencesOfString: aString [
		"Count the non-overlapping occurrences of aString in the receiver."
		| count index found |
		aString isEmpty ifTrue: [^0].
		count := 0.
		index := 1.
		[found := self indexOfSubCollection: aString startingAt: index. found > 0]
			whileTrue: [
				count := count + 1.
				index := found + aString size].
		^count
	]


	"converting"

	asSymbol [
		<primitive: StringAsSymbolPrimitive>
	]


	asString [
		"A String is already a string; answer self. Symbol overrides this to answer
		 a fresh, mutable, plain String (dropping its interned/immutable identity)."
		^self
	]


	asLowercase [
		"C fast path: pure ASCII fold (A-Z -> a-z). The Smalltalk fallback runs
		 for non-String byte shapes (e.g. Symbol), building a plain String via the
		 receiver species (a Symbol answers a String, since it is immutable)."
		<primitive: StringAsciiLowercasePrimitive>
		| newString |
		newString := self species new: self size.
		1 to: self size do: [ :i | newString at: i put: (self at: i) asLowercase].
		^newString
	]


	asUppercase [
		<primitive: StringAsciiUppercasePrimitive>
		| newString |
		newString := self species new: self size.
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


	"formatting and manipulation"

	format: aCollection [
		"Answer a copy with each {n} placeholder replaced by the displayString of
		 the n-th element of aCollection (1-based). A brace run that is not a valid
		 {digits} placeholder is copied verbatim."
		^String streamContents: [ :ws | | i sz |
			i := 1.
			sz := self size.
			[i <= sz] whileTrue: [ | ch |
				ch := self at: i.
				(ch = ${ and: [i < sz and: [(self at: i + 1) isDigit]])
					ifTrue: [ | j num |
						j := i + 1.
						num := 0.
						[j <= sz and: [(self at: j) isDigit]] whileTrue: [
							num := (num * 10) + (self at: j) digitValue.
							j := j + 1].
						(j <= sz and: [(self at: j) = $}])
							ifTrue: [
								ws nextPutAll: (aCollection at: num) displayString.
								i := j + 1]
							ifFalse: [ws nextPut: ch. i := i + 1]]
					ifFalse: [ws nextPut: ch. i := i + 1]]]
	]


	% aCollection [
		"Alias for format:, reading like a printf template: '{1}!' % arr."
		^self format: aCollection
	]


	replaceAll: oldChar with: newChar [
		"Replace every element equal to oldChar with newChar IN PLACE; answer self."
		1 to: self size do: [ :i |
			(self at: i) = oldChar ifTrue: [self at: i put: newChar]].
		^self
	]


	padLeftTo: aWidth [
		^self padLeftTo: aWidth with: Character space
	]


	padRightTo: aWidth [
		^self padRightTo: aWidth with: Character space
	]


	padLeftTo: aWidth with: aCharacter [
		"Right-justify: pad with aCharacter on the left up to aWidth. When already
		 at least aWidth long, answer the receiver unchanged."
		| pad result n |
		n := self size.
		n >= aWidth ifTrue: [^self].
		pad := aWidth - n.
		result := String new: aWidth.
		1 to: pad do: [ :i | result at: i put: aCharacter].
		result replaceFrom: pad + 1 to: aWidth with: self startingAt: 1.
		^result
	]


	padRightTo: aWidth with: aCharacter [
		"Left-justify: pad with aCharacter on the right up to aWidth."
		| result n |
		n := self size.
		n >= aWidth ifTrue: [^self].
		result := String new: aWidth.
		result replaceFrom: 1 to: n with: self startingAt: 1.
		n + 1 to: aWidth do: [ :i | result at: i put: aCharacter].
		^result
	]


	substrings [
		"Split on runs of whitespace, dropping empty pieces. Answer an
		 OrderedCollection of Strings."
		^self substringsSatisfying: [ :c | c isSeparator]
	]


	substrings: separators [
		"Split on any Character contained in separators, dropping empty pieces."
		^self substringsSatisfying: [ :c | separators includes: c]
	]


	substringsSatisfying: aBlock [
		"Private tokenizer: split into the maximal runs of characters for which
		 aBlock answers false, dropping the separators."
		| result start n |
		result := OrderedCollection new.
		n := self size.
		start := nil.
		1 to: n do: [ :i |
			(aBlock value: (self at: i))
				ifTrue: [start notNil ifTrue: [result add: (self copyFrom: start to: i - 1). start := nil]]
				ifFalse: [start isNil ifTrue: [start := i]]].
		start notNil ifTrue: [result add: (self copyFrom: start to: n)].
		^result
	]


	lines [
		"Split into lines on LF, tolerating a CR before the LF (CRLF). A trailing
		 LF does not produce a final empty line."
		| result start n |
		result := OrderedCollection new.
		n := self size.
		start := 1.
		1 to: n do: [ :i |
			(self at: i) codePoint = 10 ifTrue: [ | stop |
				stop := i - 1.
				(stop >= start and: [(self at: stop) codePoint = 13]) ifTrue: [stop := stop - 1].
				result add: (self copyFrom: start to: stop).
				start := i + 1]].
		start <= n ifTrue: [ | stop |
			stop := n.
			(stop >= start and: [(self at: stop) codePoint = 13]) ifTrue: [stop := stop - 1].
			result add: (self copyFrom: start to: stop)].
		^result
	]


	join: aCollection [
		"Answer the elements of aCollection joined into one String with the
		 receiver as separator: ', ' join: #('a' 'b') answers 'a, b'."
		^String streamContents: [ :ws | | first |
			first := true.
			aCollection do: [ :each |
				first ifTrue: [first := false] ifFalse: [ws nextPutAll: self].
				ws nextPutAll: each displayString]]
	]


	trimLeft [
		"Answer a copy with leading separators removed."
		1 to: self size do: [ :i |
			(self at: i) isSeparator ifFalse: [^self copyFrom: i to: self size]].
		^''
	]


	trimRight [
		"Answer a copy with trailing separators removed."
		self size to: 1 by: -1 do: [ :i |
			(self at: i) isSeparator ifFalse: [^self copyFrom: 1 to: i]].
		^''
	]


	trim [
		"Alias for trimSeparators: strip separators from both ends."
		^self trimSeparators
	]


	capitalized [
		"Answer a copy with the first character upper-cased; the rest is untouched."
		| result |
		self isEmpty ifTrue: [^self asString].
		result := self copyFrom: 1 to: self size.
		result at: 1 put: (result at: 1) asUppercase.
		^result
	]


	asLowercaseFirst [
		"Answer a copy with the first character lower-cased; the rest is untouched."
		| result |
		self isEmpty ifTrue: [^self asString].
		result := self copyFrom: 1 to: self size.
		result at: 1 put: (result at: 1) asLowercase.
		^result
	]


	repeat: anInteger [
		"Answer the receiver concatenated anInteger times ('ab' repeat: 3 -> 'ababab')."
		^String streamContents: [ :ws | anInteger timesRepeat: [ws nextPutAll: self]]
	]


	asByteArray [
		"Answer a ByteArray of the receiver's raw code points."
		| n ba |
		n := self size.
		ba := ByteArray new: n.
		1 to: n do: [ :i | ba at: i put: (self at: i) codePoint].
		^ba
	]


	asInteger [
		"Parse a leading optional sign and decimal digits, answering the Integer,
		 or nil when the (trimmed) receiver is not a plain integer literal."
		| s n i sign value |
		s := self trimSeparators.
		n := s size.
		n = 0 ifTrue: [^nil].
		i := 1.
		sign := 1.
		(s at: 1) = $- ifTrue: [sign := -1. i := 2].
		(s at: 1) = $+ ifTrue: [i := 2].
		i > n ifTrue: [^nil].
		value := 0.
		i to: n do: [ :k | | c |
			c := s at: k.
			c isDigit ifFalse: [^nil].
			value := (value * 10) + c digitValue].
		^value * sign
	]


	asDateTime [
		"Parse an ISO 8601 date/time string (see DateTime class>>fromString:)."
		^DateTime fromString: self
	]


	asDate [
		"Parse an ISO 8601 string and answer its Date part."
		^self asDateTime asDate
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


	"displaying"

	displayString [
		"Answer the receiver itself: the raw characters, without the surrounding
		 quotes that printOn: adds."
		^self
	]


	displayOn: aStream [
		aStream nextPutAll: self.
	]

]
