"A small, self-contained JSON codec.

 Json encode: anObject renders a Smalltalk object as a JSON string:
   Dictionary              -> object
   OrderedCollection/Array -> array
   String / Symbol         -> quoted, escaped string
   Integer / Float         -> number
   true / false            -> literal
   nil                     -> null
 Anything else is rendered as the JSON string of its printString.

 Json parse: aString is a recursive-descent parser that returns Dictionaries
 for objects, OrderedCollections for arrays, Strings, Numbers, true/false and nil."
Json := Object [

	"====================================================================
	  Encoding
	  ==================================================================="

	class encode: anObject [
		| stream |
		stream := CollectionStream with: (String new: 64).
		self encode: anObject on: stream.
		^stream contents
	]


	class encode: anObject on: aStream [
		anObject isNil   ifTrue: [ ^aStream nextPutAll: 'null' ].
		anObject == true  ifTrue: [ ^aStream nextPutAll: 'true' ].
		anObject == false ifTrue: [ ^aStream nextPutAll: 'false' ].
		(anObject isKindOf: String)     ifTrue: [ ^self encodeString: anObject on: aStream ].
		(anObject isKindOf: Number)     ifTrue: [ ^aStream nextPutAll: anObject printString ].
		(anObject isKindOf: Dictionary) ifTrue: [ ^self encodeDictionary: anObject on: aStream ].
		(anObject isKindOf: Collection) ifTrue: [ ^self encodeArray: anObject on: aStream ].
		^self encodeString: anObject printString on: aStream
	]


	class encodeDictionary: aDictionary on: aStream [
		| first |
		aStream nextPut: ${.
		first := true.
		aDictionary keysAndValuesDo: [:key :value |
			first ifTrue: [ first := false ] ifFalse: [ aStream nextPut: $, ].
			self encodeString: ((key isKindOf: String) ifTrue: [ key ] ifFalse: [ key printString ])
				on: aStream.
			aStream nextPut: $:.
			self encode: value on: aStream ].
		aStream nextPut: $}.
		^aStream
	]


	class encodeArray: aCollection on: aStream [
		| first |
		aStream nextPut: $[.
		first := true.
		aCollection do: [:value |
			first ifTrue: [ first := false ] ifFalse: [ aStream nextPut: $, ].
			self encode: value on: aStream ].
		aStream nextPut: $].
		^aStream
	]


	class encodeString: aString on: aStream [
		aStream nextPut: $".
		aString do: [:ch | self encodeChar: ch on: aStream ].
		aStream nextPut: $".
		^aStream
	]


	class encodeChar: ch on: aStream [
		| code |
		ch = $" ifTrue: [ ^aStream nextPutAll: '\"' ].
		ch = $\ ifTrue: [ ^aStream nextPutAll: '\\' ].
		code := ch codePoint.
		code = 10 ifTrue: [ ^aStream nextPutAll: '\n' ].
		code = 13 ifTrue: [ ^aStream nextPutAll: '\r' ].
		code = 9  ifTrue: [ ^aStream nextPutAll: '\t' ].
		code = 8  ifTrue: [ ^aStream nextPutAll: '\b' ].
		code = 12 ifTrue: [ ^aStream nextPutAll: '\f' ].
		code < 32 ifTrue: [ ^self encodeControl: code on: aStream ].
		^aStream nextPut: ch
	]


	class encodeControl: code on: aStream [
		aStream nextPutAll: '\u00'.
		aStream nextPut: (Character digitValue: code // 16).
		aStream nextPut: (Character digitValue: code \\ 16).
		^aStream
	]


	"====================================================================
	  Parsing
	  ==================================================================="

	class parse: aString [
		^self parseValue: (CollectionStream on: aString)
	]


	class parseValue: s [
		| ch |
		self skipWhitespace: s.
		ch := s peek.
		ch isNil ifTrue: [ ^nil ].
		ch = ${ ifTrue: [ ^self parseObject: s ].
		ch = $[ ifTrue: [ ^self parseArray: s ].
		ch = $" ifTrue: [ ^self parseString: s ].
		ch = $t ifTrue: [ ^self parseLiteral: s size: 4 value: true ].
		ch = $f ifTrue: [ ^self parseLiteral: s size: 5 value: false ].
		ch = $n ifTrue: [ ^self parseLiteral: s size: 4 value: nil ].
		^self parseNumber: s
	]


	class parseObject: s [
		| dict key |
		dict := Dictionary new.
		s next.                                  "consume ${"
		self skipWhitespace: s.
		s peek = $} ifTrue: [ s next. ^dict ].
		[
			self skipWhitespace: s.
			key := self parseString: s.
			self skipWhitespace: s.
			s next.                              "consume $:"
			dict at: key put: (self parseValue: s).
			self skipWhitespace: s.
			s peek = $, ifTrue: [ s next ] ifFalse: [ s next. ^dict ]
		] repeat.
	]


	class parseArray: s [
		| array |
		array := OrderedCollection new.
		s next.                                  "consume $["
		self skipWhitespace: s.
		s peek = $] ifTrue: [ s next. ^array ].
		[
			array addLast: (self parseValue: s).
			self skipWhitespace: s.
			s peek = $, ifTrue: [ s next ] ifFalse: [ s next. ^array ]
		] repeat.
	]


	class parseString: s [
		| out ch |
		out := CollectionStream with: (String new: 16).
		s next.                                  "consume opening quote"
		[
			ch := s next.
			ch isNil ifTrue: [ ^out contents ].
			ch = $" ifTrue: [ ^out contents ].
			ch = $\
				ifTrue: [ out nextPut: (self parseEscape: s) ]
				ifFalse: [ out nextPut: ch ]
		] repeat.
	]


	class parseEscape: s [
		| ch |
		ch := s next.
		ch = $n ifTrue: [ ^Character lf ].
		ch = $t ifTrue: [ ^Character tab ].
		ch = $r ifTrue: [ ^Character cr ].
		ch = $b ifTrue: [ ^Character codePoint: 8 ].
		ch = $f ifTrue: [ ^Character codePoint: 12 ].
		ch = $u ifTrue: [ ^self parseUnicode: s ].
		"backslash, slash, quote and any unknown escape: keep the literal char"
		^ch
	]


	class parseUnicode: s [
		| code |
		code := 0.
		4 timesRepeat: [ code := (code * 16) + s next asUppercase digitValue ].
		^Character codePoint: code
	]


	class parseNumber: s [
		| out ch |
		out := CollectionStream with: (String new: 16).
		[ ch := s peek. ch notNil and: [ self isNumberChar: ch ] ]
			whileTrue: [ out nextPut: s next ].
		^out contents asNumber
	]


	class parseLiteral: s size: n value: aValue [
		n timesRepeat: [ s next ].
		^aValue
	]


	"private"

	class skipWhitespace: s [
		[ s atEnd not and: [ s peek isSeparator ] ] whileTrue: [ s next ].
		^self
	]


	class isNumberChar: ch [
		ch isDigit ifTrue: [ ^true ].
		ch = $- ifTrue: [ ^true ].
		ch = $+ ifTrue: [ ^true ].
		ch = $. ifTrue: [ ^true ].
		ch = $e ifTrue: [ ^true ].
		ch = $E ifTrue: [ ^true ].
		^false
	]

]
