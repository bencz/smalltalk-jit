"Tests for the Json codec: encoding, parsing, escaping and round-trips.
 All blocks share one TestRun through the global JsonRun; the last block
 returns the report."

"encode primitives"
[
	| t |
	t := TestRun named: 'Json'.
	JsonRun := t.
	t check: 'encode integer' equals: '42' is: [ Json encode: 42 ].
	t check: 'encode true' equals: 'true' is: [ Json encode: true ].
	t check: 'encode false' equals: 'false' is: [ Json encode: false ].
	t check: 'encode nil' equals: 'null' is: [ Json encode: nil ].
	t check: 'encode plain string' equals: '"hi"' is: [ Json encode: 'hi' ].
]

"encode a string with characters that must be escaped"
[
	| t s |
	t := JsonRun.
	s := String new: 3.
	s at: 1 put: $a. s at: 2 put: Character lf. s at: 3 put: $b.
	t check: 'encode escapes lf' equals: '"a\nb"' is: [ Json encode: s ].
	t check: 'encode escapes quote and backslash'
		equals: '"quote \" and slash \\ end"'
		is: [ Json encode: 'quote " and slash \ end' ].
]

"encode an array"
[
	| t a |
	t := JsonRun.
	a := OrderedCollection new.
	a add: 1; add: 2; add: 3.
	t check: 'encode collection as array' equals: '[1,2,3]' is: [ Json encode: a ].
]

"parse primitives"
[
	| t |
	t := JsonRun.
	t check: 'parse integer' equals: 42 is: [ Json parse: '42' ].
	t check: 'parse true' equals: true is: [ Json parse: 'true' ].
	t check: 'parse false' equals: false is: [ Json parse: 'false' ].
	t check: 'parse null is nil' that: [ (Json parse: 'null') isNil ].
	t check: 'parse padded string' equals: 'hello' is: [ Json parse: '  "hello"  ' ].
]

"parse an array"
[
	| t a |
	t := JsonRun.
	a := Json parse: '[1, 2, 3]'.
	t check: 'parsed array size' equals: 3 is: [ a size ].
	t check: 'parsed array first' equals: 1 is: [ a at: 1 ].
	t check: 'parsed array last' equals: 3 is: [ a at: 3 ].
]

"parse an object"
[
	| t d |
	t := JsonRun.
	d := Json parse: '{"name": "Alice", "age": 30, "active": true}'.
	t check: 'parsed object string value' equals: 'Alice' is: [ d at: 'name' ].
	t check: 'parsed object number value' equals: 30 is: [ d at: 'age' ].
	t check: 'parsed object boolean value' equals: true is: [ d at: 'active' ].
]

"parse a nested object with an array of objects"
[
	| t d cards |
	t := JsonRun.
	d := Json parse: '{"id": 7, "cards": [{"t": "a"}, {"t": "b"}]}'.
	t check: 'nested object id' equals: 7 is: [ d at: 'id' ].
	cards := d at: 'cards'.
	t check: 'nested array size' equals: 2 is: [ cards size ].
	t check: 'nested array first element key' equals: 'a' is: [ (cards at: 1) at: 't' ].
	t check: 'nested array second element key' equals: 'b' is: [ (cards at: 2) at: 't' ].
]

"parse string escapes"
[
	| t s |
	t := JsonRun.
	s := Json parse: '"a\nb\tc\"d"'.
	t check: 'escaped string size' equals: 7 is: [ s size ].
	t check: 'escaped lf decoded' equals: Character lf is: [ s at: 2 ].
	t check: 'escaped tab decoded' equals: Character tab is: [ s at: 4 ].
	t check: 'escaped quote decoded' equals: $" is: [ s at: 6 ].
]

"round-trip a dictionary through encode then parse"
[
	| t d back |
	t := JsonRun.
	d := Dictionary new.
	d at: 'title' put: 'Founder'.
	d at: 'views' put: 99.
	d at: 'active' put: true.
	back := Json parse: (Json encode: d).
	t check: 'round-trip string value' equals: 'Founder' is: [ back at: 'title' ].
	t check: 'round-trip number value' equals: 99 is: [ back at: 'views' ].
	t check: 'round-trip boolean value' equals: true is: [ back at: 'active' ].
]

"====================================================================
  Expanded suite: validation, unicode, numbers, reflective + typed
  ===================================================================="

"---- test fixtures ----"
JsonTestAddress := Object [
	| street city |
	street [ ^street ]
	city [ ^city ]
	setStreet: s city: c [ street := s. city := c ]
]

JsonTestTag := Object [
	| label |
	label [ ^label ]
	setLabel: l [ label := l ]
]

JsonTestPerson := Object [
	| name age address tags |
	name [ ^name ]
	age [ ^age ]
	address [ ^address ]
	tags [ ^tags ]
	setName: n age: a [ name := n. age := a ]
	setAddress: ad [ address := ad ]
	setTags: t [ tags := t ]

	class jsonTypes [
		| t |
		t := Dictionary new.
		t at: 'address' put: JsonTestAddress.
		t at: 'tags' put: (Array with: JsonTestTag).
		^t
	]
]

JsonTestCustom := Object [
	| value |
	setValue: v [ value := v ]
	jsonOn: aStream [ aStream nextPutAll: '"custom!"' ]
]

"malformed input raises JsonParseError"
[
	| t |
	t := JsonRun.
	t expect: 'reject empty input' raises: JsonParseError in: [ Json parse: '' ].
	t expect: 'reject blank input' raises: JsonParseError in: [ Json parse: '   ' ].
	t expect: 'reject truncated true' raises: JsonParseError in: [ Json parse: 'tru' ].
	t expect: 'reject true with trailing garbage' raises: JsonParseError in: [ Json parse: 'truex' ].
	t expect: 'reject mixed-case true' raises: JsonParseError in: [ Json parse: 'tRuE' ].
	t expect: 'reject truncated null' raises: JsonParseError in: [ Json parse: 'nul' ].
	t expect: 'reject lone open brace' raises: JsonParseError in: [ Json parse: '{' ].
	t expect: 'reject truncated array' raises: JsonParseError in: [ Json parse: '[1,' ].
	t expect: 'reject key without value' raises: JsonParseError in: [ Json parse: '{"a"}' ].
	t expect: 'reject colon without value' raises: JsonParseError in: [ Json parse: '{"a":}' ].
	t expect: 'reject trailing comma in object' raises: JsonParseError in: [ Json parse: '{"a":1,}' ].
	t expect: 'reject missing comma in object' raises: JsonParseError in: [ Json parse: '{"a":1 "b":2}' ].
	t expect: 'reject non-string key' raises: JsonParseError in: [ Json parse: '{1:2}' ].
	t expect: 'reject missing comma in array' raises: JsonParseError in: [ Json parse: '[1 2]' ].
	t expect: 'reject lone close brace' raises: JsonParseError in: [ Json parse: '}' ].
	t expect: 'reject two top-level values' raises: JsonParseError in: [ Json parse: 'null null' ].
	t expect: 'reject trailing garbage after object' raises: JsonParseError in: [ Json parse: '{"a":1} x' ].
	t expect: 'reject unterminated string' raises: JsonParseError in: [ Json parse: '"aberta' ].
	t expect: 'reject unknown escape' raises: JsonParseError in: [ Json parse: '"\q"' ].
]

"parse error carries the 1-based position"
[
	| t |
	t := JsonRun.
	t check: 'error position for trailing comma' equals: 4 is: [
		[ Json parse: '[1,]'. #noRaise ] on: JsonParseError do: [:e | e position ] ].
	t check: 'error position for bad literal' equals: 4 is: [
		[ Json parse: 'nulx'. #noRaise ] on: JsonParseError do: [:e | e position ] ].
]

"raw control characters inside strings are rejected"
[
	| t s |
	t := JsonRun.
	s := String new: 3.
	s at: 1 put: $".
	s at: 2 put: (Character codePoint: 7).
	s at: 3 put: $".
	t expect: 'reject raw control character in string' raises: JsonParseError in: [ Json parse: s ].
]

"unicode escapes: lone/bad surrogates and bad hex are rejected"
[
	| t |
	t := JsonRun.
	t expect: 'reject lone high surrogate' raises: JsonParseError in: [ Json parse: '"\uD800"' ].
	t expect: 'reject high surrogate pair' raises: JsonParseError in: [ Json parse: '"\uD800\uD800"' ].
	t expect: 'reject lone low surrogate' raises: JsonParseError in: [ Json parse: '"\uDC00"' ].
	t expect: 'reject bad hex digit in escape' raises: JsonParseError in: [ Json parse: '"\u12G4"' ].
	t expect: 'reject short unicode escape' raises: JsonParseError in: [ Json parse: '"\u12"' ].
]

"nesting deeper than maxDepth is rejected (not a hang, not a crash)"
[
	| t s |
	t := JsonRun.
	s := String new: 200.
	1 to: 200 do: [:i | s at: i put: $[ ].
	t expect: 'reject nesting beyond maxDepth' raises: JsonParseError in: [ Json parse: s ].
]

"invalid numbers are rejected"
[
	| t |
	t := JsonRun.
	t expect: 'reject leading zero' raises: JsonParseError in: [ Json parse: '01' ].
	t expect: 'reject leading plus' raises: JsonParseError in: [ Json parse: '+1' ].
	t expect: 'reject bare fraction' raises: JsonParseError in: [ Json parse: '.5' ].
	t expect: 'reject trailing dot' raises: JsonParseError in: [ Json parse: '1.' ].
	t expect: 'reject empty exponent' raises: JsonParseError in: [ Json parse: '1e' ].
	t expect: 'reject signed empty exponent' raises: JsonParseError in: [ Json parse: '1e+' ].
	t expect: 'reject lone minus' raises: JsonParseError in: [ Json parse: '-' ].
	t expect: 'reject minus with space' raises: JsonParseError in: [ Json parse: '- 1' ].
]

"valid numbers: zero forms, exponents, floats"
[
	| t |
	t := JsonRun.
	t check: 'parse negative zero' equals: 0 is: [ Json parse: '-0' ].
	t check: 'parse zero' equals: 0 is: [ Json parse: '0' ].
	t check: 'parse simple float' equals: 0.5 is: [ Json parse: '0.5' ].
	t check: 'parse negative float' equals: 2.75 negated is: [ Json parse: '-2.75' ].
	t check: 'parse exponent' equals: 100 is: [ Json parse: '1e2' ].
	t check: 'parse negative exponent' equals: 0.01 is: [ Json parse: '1E-2' ].
	t check: 'parse plus exponent' equals: 300 is: [ Json parse: '3e+2' ].
]

"float round-trips through encode then parse"
[
	| t |
	t := JsonRun.
	t check: 'float round-trip 0.1' equals: 0.1 is: [ Json parse: (Json encode: 0.1) ].
	t check: 'float round-trip 1.5' equals: 1.5 is: [ Json parse: (Json encode: 1.5) ].
	t check: 'float round-trip 1.0e300' equals: 1.0e300 is: [ Json parse: (Json encode: 1.0e300) ].
	t check: 'float round-trip 0.1 + 0.2' equals: 0.1 + 0.2 is: [ Json parse: (Json encode: 0.1 + 0.2) ].
]

"big integers promote to LargeInteger and round-trip"
[
	| t big back |
	t := JsonRun.
	big := Json parse: '123456789012345678901234567890'.
	t check: 'big integer parses' equals: '123456789012345678901234567890' is: [ big printString ].
	back := Json parse: (Json encode: big).
	t check: 'big integer round-trips' equals: big is: [ back ].
	t check: 'negative big integer parses' equals: big negated is: [
		Json parse: '-123456789012345678901234567890' ].
]

"unicode escapes decode to UTF-8 bytes (surrogate pairs included)"
[
	| t s |
	t := JsonRun.
	s := Json parse: '"\u0041"'.
	t check: 'u0041 decodes to A' equals: 'A' is: [ s ].
	s := Json parse: '"\u00E9"'.
	t check: 'u00E9 utf8 length' equals: 2 is: [ s size ].
	t check: 'u00E9 utf8 byte 1' equals: 195 is: [ (s at: 1) codePoint ].
	t check: 'u00E9 utf8 byte 2' equals: 169 is: [ (s at: 2) codePoint ].
	s := Json parse: '"\u20AC"'.
	t check: 'u20AC utf8 length' equals: 3 is: [ s size ].
	t check: 'u20AC utf8 byte 1' equals: 226 is: [ (s at: 1) codePoint ].
	s := Json parse: '"\uD83D\uDE00"'.
	t check: 'surrogate pair utf8 length' equals: 4 is: [ s size ].
	t check: 'surrogate pair byte 1' equals: 240 is: [ (s at: 1) codePoint ].
	t check: 'surrogate pair byte 2' equals: 159 is: [ (s at: 2) codePoint ].
	t check: 'surrogate pair byte 3' equals: 152 is: [ (s at: 3) codePoint ].
	t check: 'surrogate pair byte 4' equals: 128 is: [ (s at: 4) codePoint ].
	s := Json parse: '"\u0000"'.
	t check: 'u0000 length' equals: 1 is: [ s size ].
	t check: 'u0000 byte' equals: 0 is: [ (s at: 1) codePoint ].
	t check: 'escaped slash decodes' equals: '/' is: [ Json parse: '"\/"' ].
]

"encoder: NaN and Infinity are errors; Fraction converts to float"
[
	| t |
	t := JsonRun.
	t expect: 'encode NaN raises' raises: JsonError in: [ Json encode: Float nan ].
	t expect: 'encode Infinity raises' raises: JsonError in: [ Json encode: Float infinity ].
	t expect: 'encode negative Infinity raises' raises: JsonError in: [ Json encode: Float infinity negated ].
	t check: 'encode fraction 1/2' equals: '0.5' is: [ Json encode: 1 / 2 ].
	t check: 'encode fraction 3/4' equals: '0.75' is: [ Json encode: 3 / 4 ].
]

"encoder: symbols, characters, non-string dictionary keys"
[
	| t d |
	t := JsonRun.
	t check: 'encode symbol as string' equals: '"hello"' is: [ Json encode: #hello ].
	t check: 'encode character as string' equals: '"a"' is: [ Json encode: $a ].
	d := Dictionary new.
	d at: 1 put: 'one'.
	t check: 'encode non-string key as string' equals: '{"1":"one"}' is: [ Json encode: d ].
]

"encoder: a cyclic structure fails with JsonError instead of hanging"
[
	| t a |
	t := JsonRun.
	a := OrderedCollection new.
	a add: a.
	t expect: 'encode cyclic structure raises' raises: JsonError in: [ Json encode: a ].
]

"decode: is an alias of parse:"
[
	| t |
	t := JsonRun.
	t check: 'decode: parses a number' equals: 42 is: [ Json decode: '42' ].
	t check: 'decode: agrees with parse:' that: [
		((Json decode: '{"k":1}') at: 'k') = ((Json parse: '{"k":1}') at: 'k') ].
]

"reflective encode of a plain object: one key per instance variable"
[
	| t p json back |
	t := JsonRun.
	p := JsonTestAddress new.
	p setStreet: 'Rua A' city: 'SP'.
	json := Json encode: p.
	back := Json parse: json.
	t check: 'reflective encode street ivar' equals: 'Rua A' is: [ back at: 'street' ].
	t check: 'reflective encode city ivar' equals: 'SP' is: [ back at: 'city' ].
]

"jsonOn: hook takes full control of an object's JSON shape"
[
	| t c |
	t := JsonRun.
	c := JsonTestCustom new.
	c setValue: 42.
	t check: 'jsonOn: hook controls encoding' equals: '"custom!"' is: [ Json encode: c ].
]

"typed decode: flat object"
[
	| t p |
	t := JsonRun.
	p := Json decode: '{"name":"Alice","age":30}' as: JsonTestAddress.
	"keys that do not match ivars are ignored; unmatched ivars stay nil"
	t check: 'typed decode ignores unknown keys (street nil)' that: [ p street isNil ].
	t check: 'typed decode ignores unknown keys (city nil)' that: [ p city isNil ].
	p := Json decode: '{"street":"Rua B","city":"Rio"}' as: JsonTestAddress.
	t check: 'typed decode fills street' equals: 'Rua B' is: [ p street ].
	t check: 'typed decode fills city' equals: 'Rio' is: [ p city ].
]

"typed decode: nested objects and collections via jsonTypes"
[
	| t p |
	t := JsonRun.
	p := Json decode: '{"name":"Bob","age":41,"address":{"street":"Rua C","city":"BH"},"tags":[{"label":"a"},{"label":"b"}]}'
		as: JsonTestPerson.
	t check: 'typed nested name' equals: 'Bob' is: [ p name ].
	t check: 'typed nested age' equals: 41 is: [ p age ].
	t check: 'typed nested address class' that: [ p address isKindOf: JsonTestAddress ].
	t check: 'typed nested address city' equals: 'BH' is: [ p address city ].
	t check: 'typed nested tags size' equals: 2 is: [ p tags size ].
	t check: 'typed nested tag class' that: [ (p tags at: 1) isKindOf: JsonTestTag ].
	t check: 'typed nested tag label' equals: 'b' is: [ (p tags at: 2) label ].
]

"typed decode: null -> nil, missing -> nil, extra keys ignored"
[
	| t p |
	t := JsonRun.
	p := Json decode: '{"name":null,"age":7,"extra":true}' as: JsonTestPerson.
	t check: 'typed decode null becomes nil' that: [ p name isNil ].
	t check: 'typed decode keeps present field' equals: 7 is: [ p age ].
	t check: 'typed decode missing address nil' that: [ p address isNil ].
	t check: 'typed decode missing tags nil' that: [ p tags isNil ].
]

"typed decode of arrays: decode:asArrayOf:"
[
	| t list |
	t := JsonRun.
	list := Json decode: '[{"label":"x"},{"label":"y"}]' asArrayOf: JsonTestTag.
	t check: 'asArrayOf: size' equals: 2 is: [ list size ].
	t check: 'asArrayOf: first label' equals: 'x' is: [ (list at: 1) label ].
	t check: 'asArrayOf: second label' equals: 'y' is: [ (list at: 2) label ].
]

"object -> JSON -> object round-trip with nesting"
[
	| t p json back |
	t := JsonRun.
	p := JsonTestPerson new.
	p setName: 'Eva' age: 28.
	p setAddress: (JsonTestAddress new setStreet: 'Rua D' city: 'POA'; yourself).
	p setTags: (Array with: (JsonTestTag new setLabel: 'k'; yourself)).
	json := Json encode: p.
	back := Json decode: json as: JsonTestPerson.
	t check: 'object round-trip name' equals: 'Eva' is: [ back name ].
	t check: 'object round-trip nested street' equals: 'Rua D' is: [ back address street ].
	t check: 'object round-trip tag label' equals: 'k' is: [ (back tags at: 1) label ].
]

"whitespace tolerance and BOM rejection"
[
	| t bom |
	t := JsonRun.
	t check: 'whitespace tolerated inside array' equals: 2 is: [ (Json parse: '  [ 1 , 2 ]  ') size ].
	bom := String new: 7.
	bom at: 1 put: (Character codePoint: 239).
	bom at: 2 put: (Character codePoint: 187).
	bom at: 3 put: (Character codePoint: 191).
	bom at: 4 put: $t. bom at: 5 put: $r. bom at: 6 put: $u. bom at: 7 put: $e.
	t expect: 'reject UTF-8 BOM' raises: JsonParseError in: [ Json parse: bom ].
]

"C parser and Smalltalk reference parser agree (structural deep equality;
 dictionary ITERATION order may differ between the two, JSON objects are
 unordered)"
[
	| t docs deepEq |
	t := JsonRun.
	deepEq := nil.
	deepEq := [:x :y |
		(x isKindOf: Dictionary)
			ifTrue: [
				| same |
				same := (y isKindOf: Dictionary) and: [ x size = y size ].
				same ifTrue: [
					x keysAndValuesDo: [:k :v |
						same := same and: [ deepEq value: v value: (y at: k ifAbsent: [ #jsonTestMissing ]) ] ] ].
				same ]
			ifFalse: [
				(x isKindOf: OrderedCollection)
					ifTrue: [
						| same |
						same := (y isKindOf: OrderedCollection) and: [ x size = y size ].
						same ifTrue: [
							1 to: x size do: [:i |
								same := same and: [ deepEq value: (x at: i) value: (y at: i) ] ] ].
						same ]
					ifFalse: [ x = y ] ] ].
	docs := OrderedCollection new.
	docs add: '{"a":1,"b":[1,2,3],"c":{"d":null,"e":true},"f":"x\ny"}'.
	docs add: '[0.5,1e2,-7,"x",[],{},false]'.
	docs add: '"plain"'.
	docs add: '  {  "s p a c e" : [ 1 , { "k" : "v" } ] }  '.
	docs do: [:doc |
		t check: 'parsers agree on ', doc that: [
			deepEq value: (Json parse: doc) value: (Json new slowParse: doc) ] ].
]

"GC stress: large array, large string, many keys (scavenges mid-parse)"
[
	| t out big parsed s dict i |
	t := JsonRun.
	out := CollectionStream with: (String new: 64).
	out nextPut: $[.
	1 to: 10000 do: [:k |
		k > 1 ifTrue: [ out nextPut: $, ].
		out nextPutAll: k printString ].
	out nextPut: $].
	parsed := Json parse: out contents.
	t check: 'large array size' equals: 10000 is: [ parsed size ].
	t check: 'large array first' equals: 1 is: [ parsed at: 1 ].
	t check: 'large array last' equals: 10000 is: [ parsed at: 10000 ].

	s := 'abcdefghijklmnop'.
	1 to: 16 do: [:k | s := s , s ].               "16 doublings -> 1MB"
	big := Json parse: '"' , s , '"'.
	t check: 'large string parses at full size' equals: s size is: [ big size ].
	t check: 'large string round-trips' equals: s size is: [ (Json parse: (Json encode: big)) size ].

	out := CollectionStream with: (String new: 64).
	out nextPut: ${.
	1 to: 5000 do: [:k |
		k > 1 ifTrue: [ out nextPut: $, ].
		out nextPutAll: '"k'; nextPutAll: k printString; nextPutAll: '":'.
		out nextPutAll: k printString ].
	out nextPut: $}.
	dict := Json parse: out contents.
	i := 0.
	dict keysAndValuesDo: [:k :v | i := i + 1 ].
	t check: 'many-key object key count' equals: 5000 is: [ i ].
	t check: 'many-key object first key' equals: 1 is: [ dict at: 'k1' ].
	t check: 'many-key object last key' equals: 5000 is: [ dict at: 'k5000' ].
]

[
	^JsonRun report
]
