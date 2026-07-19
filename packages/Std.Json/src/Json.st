"A robust, self-contained JSON codec (RFC 8259).

 Json encode: anObject renders a Smalltalk object as a JSON string:
   Dictionary              -> object
   OrderedCollection/Array -> array   (any other Collection too)
   String / Symbol         -> quoted, escaped string
   Integer                 -> number  (LargeInteger included, arbitrary precision)
   Float                   -> number  (shortest round-trip form; NaN/Infinity -> JsonError)
   Fraction                -> number  (converted with asFloat)
   Character               -> one-character string
   true / false / nil      -> true / false / null
   any other object        -> reflective {'ivarName': value, ...} via the class's
                              instanceVariables (a class may instead implement
                              jsonOn: aStream to take full control of its shape)

 Json parse: aString (alias: decode:) is a VALIDATING parser: malformed input
 raises JsonParseError with the 1-based position. Objects decode to Dictionary
 (String keys), arrays to OrderedCollection, numbers to Integer/Float (integers
 of any size promote to LargeInteger), \uXXXX escapes (surrogate pairs included)
 decode to UTF-8 bytes.

 Json decode: aString as: aClass materializes a JSON object into an instance of
 aClass by matching keys to instance variable names (missing key -> nil, extra
 keys ignored, null -> nil). Nested typed fields are declared with an optional
 class-side method jsonTypes returning a Dictionary of ivarName -> class, or
 ivarName -> (Array with: elementClass) for collections of typed elements.
 Json decode: aString asArrayOf: aClass materializes a JSON array of objects.

 Limits: nesting depth is capped at maxDepth (128) for both directions — a
 cyclic object graph fails the encoder with JsonError. The hot paths (parse of
 core types, encode of core-type graphs) run as C primitives; the Smalltalk
 code below is both the fallback (LargeInteger literals, reflective encode,
 error reporting) and the reference implementation."
JsonError := Error [
]

JsonParseError := JsonError [
	| position |

	position [ ^position ]
	setPosition: anInteger [ position := anInteger ]

	class signal: aString at: anInteger [
		| err |
		err := self new.
		err setPosition: anInteger.
		^err signal: aString, ' (at position ', anInteger printString, ')'
	]
]

Json := Object [

	| source pos |

	"====================================================================
	  Public API (class side)
	  ==================================================================="

	class maxDepth [ ^128 ]

	class parse: aString [
		<primitive: JsonParsePrimitive>
		"C fast path failed: either a syntax error (re-parse for a precise
		 JsonParseError) or an integer beyond SmallInteger (promotes here)."
		^self new slowParse: aString
	]

	class decode: aString [ ^self parse: aString ]

	class decode: aString as: aClass [
		^self materialize: (self parse: aString) as: aClass
	]

	class decode: aString asArrayOf: aClass [
		^(self parse: aString) collect: [:each | self materialize: each as: aClass ]
	]

	class encode: anObject [
		<primitive: JsonEncodePrimitive>
		"C fast path failed (a non-core class, NaN/Infinity or a cycle is in the
		 graph): walk reflectively, re-entering the primitive for core subtrees."
		| stream |
		stream := CollectionStream with: (String new: 64).
		self encode: anObject on: stream depth: 0.
		^stream contents
	]

	class encode: anObject on: aStream [
		self encode: anObject on: aStream depth: 0.
		^aStream
	]

	"====================================================================
	  Encoding
	  ==================================================================="

	"Fast path for subtrees made only of core JSON types (the C encoder walks
	 Dictionary/OrderedCollection/Array/String/Symbol/SmallInteger/Float/booleans/
	 nil without allocating). Returns the JSON String, or nil when the subtree
	 contains anything else — the caller then walks that level reflectively."
	class primEncode: anObject [
		<primitive: JsonEncodePrimitive>
		^nil
	]

	class encode: anObject on: aStream depth: d [
		| s |
		d > self maxDepth ifTrue: [
			JsonError signal: 'maximum nesting depth exceeded (cyclic object?)' ].
		s := self primEncode: anObject.
		s isNil ifFalse: [ ^aStream nextPutAll: s ].
		anObject isNil    ifTrue: [ ^aStream nextPutAll: 'null' ].
		anObject == true  ifTrue: [ ^aStream nextPutAll: 'true' ].
		anObject == false ifTrue: [ ^aStream nextPutAll: 'false' ].
		(anObject respondsTo: #jsonOn:) ifTrue: [ ^anObject jsonOn: aStream ].
		(anObject isKindOf: String)     ifTrue: [ ^self encodeString: anObject on: aStream ].
		(anObject isKindOf: Character)  ifTrue: [ ^self encodeCharacterString: anObject on: aStream ].
		(anObject isKindOf: Fraction)   ifTrue: [ ^self encode: anObject asFloat on: aStream depth: d ].
		(anObject isKindOf: Number)     ifTrue: [ ^self encodeNumber: anObject on: aStream ].
		(anObject isKindOf: Dictionary) ifTrue: [ ^self encodeDictionary: anObject on: aStream depth: d ].
		(anObject isKindOf: Collection) ifTrue: [ ^self encodeArray: anObject on: aStream depth: d ].
		^self encodeObject: anObject on: aStream depth: d
	]

	"Reflective default for plain objects: {'ivar': value, ...}. instanceVariables
	 answers the stored names Array (inherited first), whose indices line up with
	 instVarAt:, so this walk allocates nothing beyond the output."
	class encodeObject: anObject on: aStream depth: d [
		| names first |
		names := anObject class instanceVariables.
		aStream nextPut: ${.
		first := true.
		1 to: names size do: [:i |
			first ifTrue: [ first := false ] ifFalse: [ aStream nextPut: $, ].
			self encodeString: (names at: i) on: aStream.
			aStream nextPut: $:.
			self encode: (anObject instVarAt: i) on: aStream depth: d + 1 ].
		aStream nextPut: $}.
		^aStream
	]

	class encodeCharacterString: aCharacter on: aStream [
		| s |
		s := String new: 1.
		s at: 1 put: aCharacter.
		^self encodeString: s on: aStream
	]

	class encodeNumber: aNumber on: aStream [
		(aNumber isKindOf: Float) ifTrue: [
			aNumber = aNumber ifFalse: [
				JsonError signal: 'NaN has no JSON representation' ].
			(aNumber = Float infinity or: [ aNumber = Float infinity negated ]) ifTrue: [
				JsonError signal: 'Infinity has no JSON representation' ] ].
		^aStream nextPutAll: aNumber printString
	]

	class encodeDictionary: aDictionary on: aStream depth: d [
		| first |
		aStream nextPut: ${.
		first := true.
		aDictionary keysAndValuesDo: [:key :value |
			first ifTrue: [ first := false ] ifFalse: [ aStream nextPut: $, ].
			self encodeString: ((key isKindOf: String) ifTrue: [ key ] ifFalse: [ key printString ])
				on: aStream.
			aStream nextPut: $:.
			self encode: value on: aStream depth: d + 1 ].
		aStream nextPut: $}.
		^aStream
	]

	class encodeArray: aCollection on: aStream depth: d [
		| first |
		aStream nextPut: $[.
		first := true.
		aCollection do: [:value |
			first ifTrue: [ first := false ] ifFalse: [ aStream nextPut: $, ].
			self encode: value on: aStream depth: d + 1 ].
		aStream nextPut: $].
		^aStream
	]

	class encodeString: aString on: aStream [
		| size start i ch code needsEscape |
		aStream nextPut: $".
		"Bulk-write maximal runs of characters that need no escaping in one
		 memcpy-backed nextPutAll:, escaping individual characters only where
		 needed. Bytes >= 128 pass through untouched (UTF-8 payload)."
		size := aString size.
		start := 1.
		i := 1.
		[ i <= size ] whileTrue: [
			ch := aString at: i.
			code := ch codePoint.
			needsEscape := code < 32 or: [ ch = $" or: [ ch = $\ ] ].
			needsEscape ifTrue: [
				i > start ifTrue: [ aStream nextPutAll: (aString copyFrom: start to: i - 1) ].
				self encodeChar: ch on: aStream.
				start := i + 1 ].
			i := i + 1 ].
		start = 1
			ifTrue: [ aStream nextPutAll: aString ]
			ifFalse: [ start <= size ifTrue: [ aStream nextPutAll: (aString copyFrom: start to: size) ] ].
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
	  Typed decoding (pure Smalltalk policy)
	  ==================================================================="

	class materialize: aValue as: aClass [
		| inst names types key raw spec |
		aValue isNil ifTrue: [ ^nil ].
		(aClass isNil or: [ aClass == Dictionary ]) ifTrue: [ ^aValue ].
		(aValue isKindOf: Dictionary) ifFalse: [ ^aValue ].   "JSON leaf: passes through"
		inst := aClass new.
		names := aClass instanceVariables.
		types := (aClass class includesSelector: #jsonTypes)
			ifTrue: [ aClass jsonTypes ] ifFalse: [ nil ].
		1 to: names size do: [:i |
			key := names at: i.
			raw := aValue at: key ifAbsent: [ nil ].
			spec := types isNil ifTrue: [ nil ] ifFalse: [ types at: key ifAbsent: [ nil ] ].
			inst instVarAt: i put: (self coerce: raw spec: spec) ].
		^inst
	]

	class coerce: raw spec: spec [
		spec isNil ifTrue: [ ^raw ].
		raw isNil ifTrue: [ ^nil ].
		(spec isKindOf: Array) ifTrue: [
			^raw collect: [:each | self materialize: each as: (spec at: 1) ] ].
		^self materialize: raw as: spec
	]

	"====================================================================
	  Validating parser (instance side) — the reference implementation and
	  the fallback for the C parser (syntax errors re-parse here for a precise
	  JsonParseError; integers beyond SmallInteger promote via asNumber).
	  ==================================================================="

	slowParse: aString [
		| v |
		source := aString.
		pos := 1.
		(source size >= 3 and: [
			(source at: 1) codePoint = 239 and: [
			(source at: 2) codePoint = 187 and: [
			(source at: 3) codePoint = 191 ] ] ]) ifTrue: [
				self parseError: 'byte order mark is not allowed in JSON' ].
		v := self pValue: 0.
		self skipWs.
		pos <= source size ifTrue: [ self parseError: 'unexpected trailing characters' ].
		^v
	]

	parseError: aString [
		JsonParseError signal: aString at: pos
	]

	peekChar [
		^pos <= source size ifTrue: [ source at: pos ] ifFalse: [ nil ]
	]

	nextChar [
		| ch |
		ch := self peekChar.
		ch isNil ifTrue: [ self parseError: 'unexpected end of input' ].
		pos := pos + 1.
		^ch
	]

	skipWs [
		| code |
		[ pos <= source size and: [
			code := (source at: pos) codePoint.
			code = 32 or: [ code = 9 or: [ code = 10 or: [ code = 13 ] ] ] ] ]
			whileTrue: [ pos := pos + 1 ]
	]

	pValue: d [
		| ch |
		d > Json maxDepth ifTrue: [ self parseError: 'too deeply nested' ].
		self skipWs.
		ch := self peekChar.
		ch isNil ifTrue: [ self parseError: 'unexpected end of input' ].
		ch = ${ ifTrue: [ ^self pObject: d ].
		ch = $[ ifTrue: [ ^self pArray: d ].
		ch = $" ifTrue: [ ^self pString ].
		ch = $t ifTrue: [ ^self pLiteral: 'true' value: true ].
		ch = $f ifTrue: [ ^self pLiteral: 'false' value: false ].
		ch = $n ifTrue: [ ^self pLiteral: 'null' value: nil ].
		(ch = $- or: [ ch isDigit ]) ifTrue: [ ^self pNumber ].
		self parseError: 'unexpected character'
	]

	pObject: d [
		| dict key ch |
		dict := Dictionary new.
		self nextChar.                          "consume ${"
		self skipWs.
		self peekChar = $} ifTrue: [ pos := pos + 1. ^dict ].
		[
			self skipWs.
			self peekChar = $" ifFalse: [ self parseError: 'expected a string key' ].
			key := self pString.
			self skipWs.
			self nextChar = $: ifFalse: [ pos := pos - 1. self parseError: 'expected a colon' ].
			dict at: key put: (self pValue: d + 1).
			self skipWs.
			ch := self nextChar.
			ch = $} ifTrue: [ ^dict ].
			ch = $, ifFalse: [ pos := pos - 1. self parseError: 'expected a comma or closing brace' ]
		] repeat
	]

	pArray: d [
		| array ch |
		array := OrderedCollection new.
		self nextChar.                          "consume $["
		self skipWs.
		self peekChar = $] ifTrue: [ pos := pos + 1. ^array ].
		[
			array addLast: (self pValue: d + 1).
			self skipWs.
			ch := self nextChar.
			ch = $] ifTrue: [ ^array ].
			ch = $, ifFalse: [ pos := pos - 1. self parseError: 'expected a comma or closing bracket' ]
		] repeat
	]

	pLiteral: aWord value: aValue [
		1 to: aWord size do: [:i |
			self nextChar = (aWord at: i) ifFalse: [
				pos := pos - 1. self parseError: 'invalid literal' ] ].
		^aValue
	]

	pString [
		| out ch |
		out := CollectionStream with: (String new: 16).
		self nextChar.                          "consume opening quote"
		[
			ch := self nextChar.
			ch = $" ifTrue: [ ^out contents ].
			ch = $\
				ifTrue: [ self pEscapeInto: out ]
				ifFalse: [
					ch codePoint < 32 ifTrue: [
						pos := pos - 1.
						self parseError: 'unescaped control character in string' ].
					out nextPut: ch ]
		] repeat
	]

	pEscapeInto: out [
		| ch |
		ch := self nextChar.
		ch = $" ifTrue: [ ^out nextPut: $" ].
		ch = $\ ifTrue: [ ^out nextPut: $\ ].
		ch = $/ ifTrue: [ ^out nextPut: $/ ].
		ch = $n ifTrue: [ ^out nextPut: Character lf ].
		ch = $t ifTrue: [ ^out nextPut: Character tab ].
		ch = $r ifTrue: [ ^out nextPut: Character cr ].
		ch = $b ifTrue: [ ^out nextPut: (Character codePoint: 8) ].
		ch = $f ifTrue: [ ^out nextPut: (Character codePoint: 12) ].
		ch = $u ifTrue: [ ^self pUnicodeInto: out ].
		pos := pos - 1.
		self parseError: 'invalid escape sequence'
	]

	pHex4 [
		| code ch digit |
		code := 0.
		4 timesRepeat: [
			ch := self nextChar.
			digit := ch isDigit
				ifTrue: [ ch codePoint - 48 ]
				ifFalse: [
					(ch >= $a and: [ ch <= $f ]) ifTrue: [ ch codePoint - 87 ] ifFalse: [
					(ch >= $A and: [ ch <= $F ]) ifTrue: [ ch codePoint - 55 ] ifFalse: [
						pos := pos - 1.
						self parseError: 'invalid unicode escape' ] ] ].
			code := (code * 16) + digit ].
		^code
	]

	"\uXXXX (with a mandatory low surrogate after a high one) -> UTF-8 bytes."
	pUnicodeInto: out [
		| code low |
		code := self pHex4.
		(code >= 16rD800 and: [ code <= 16rDBFF ]) ifTrue: [
			(self peekChar = $\ ) ifFalse: [ self parseError: 'missing low surrogate' ].
			pos := pos + 1.
			(self peekChar = $u) ifFalse: [ self parseError: 'missing low surrogate' ].
			pos := pos + 1.
			low := self pHex4.
			(low >= 16rDC00 and: [ low <= 16rDFFF ]) ifFalse: [
				self parseError: 'invalid low surrogate' ].
			code := 16r10000 + ((code - 16rD800) * 1024) + (low - 16rDC00) ].
		(code >= 16rDC00 and: [ code <= 16rDFFF ]) ifTrue: [
			self parseError: 'lone low surrogate' ].
		self utf8: code into: out
	]

	utf8: cp into: out [
		cp < 128 ifTrue: [ ^out nextPut: (Character codePoint: cp) ].
		cp < 2048 ifTrue: [
			out nextPut: (Character codePoint: 192 + (cp // 64)).
			^out nextPut: (Character codePoint: 128 + (cp \\ 64)) ].
		cp < 65536 ifTrue: [
			out nextPut: (Character codePoint: 224 + (cp // 4096)).
			out nextPut: (Character codePoint: 128 + ((cp // 64) \\ 64)).
			^out nextPut: (Character codePoint: 128 + (cp \\ 64)) ].
		out nextPut: (Character codePoint: 240 + (cp // 262144)).
		out nextPut: (Character codePoint: 128 + ((cp // 4096) \\ 64)).
		out nextPut: (Character codePoint: 128 + ((cp // 64) \\ 64)).
		^out nextPut: (Character codePoint: 128 + (cp \\ 64))
	]

	"Strict RFC 8259 number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
	 Validated by position first, converted with asNumber after (which promotes
	 to LargeInteger / Float as needed)."
	pNumber [
		| start ch isFloat |
		start := pos.
		isFloat := false.
		self peekChar = $- ifTrue: [ pos := pos + 1 ].
		ch := self peekChar.
		(ch notNil and: [ ch isDigit ]) ifFalse: [ self parseError: 'invalid number' ].
		ch = $0
			ifTrue: [
				pos := pos + 1.
				(self peekChar notNil and: [ self peekChar isDigit ]) ifTrue: [
					self parseError: 'leading zeros are not allowed' ] ]
			ifFalse: [
				[ self peekChar notNil and: [ self peekChar isDigit ] ]
					whileTrue: [ pos := pos + 1 ] ].
		self peekChar = $. ifTrue: [
			isFloat := true.
			pos := pos + 1.
			(self peekChar notNil and: [ self peekChar isDigit ]) ifFalse: [
				self parseError: 'invalid number' ].
			[ self peekChar notNil and: [ self peekChar isDigit ] ]
				whileTrue: [ pos := pos + 1 ] ].
		(self peekChar = $e or: [ self peekChar = $E ]) ifTrue: [
			isFloat := true.
			pos := pos + 1.
			(self peekChar = $+ or: [ self peekChar = $- ]) ifTrue: [ pos := pos + 1 ].
			(self peekChar notNil and: [ self peekChar isDigit ]) ifFalse: [
				self parseError: 'invalid number' ].
			[ self peekChar notNil and: [ self peekChar isDigit ] ]
				whileTrue: [ pos := pos + 1 ] ].
		"a '.' or exponent makes it a Float, matching the C parser (strtod), even
		 when the value is integral: 1e2 -> 100.0, not 100"
		isFloat ifTrue: [ ^(source copyFrom: start to: pos - 1) asNumber asFloat ].
		^(source copyFrom: start to: pos - 1) asNumber
	]

]
