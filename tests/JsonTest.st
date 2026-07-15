"Tests for the Json codec: encoding, parsing, escaping and round-trips."

"encode primitives"
[
	Assert true: (Json encode: 42) = '42'.
	Assert true: (Json encode: true) = 'true'.
	Assert true: (Json encode: false) = 'false'.
	Assert true: (Json encode: nil) = 'null'.
	Assert true: (Json encode: 'hi') = '"hi"'.
]

"encode a string with characters that must be escaped"
[
	| s |
	s := String new: 3.
	s at: 1 put: $a. s at: 2 put: Character lf. s at: 3 put: $b.
	Assert true: (Json encode: s) = '"a\nb"'.
	Assert true: (Json encode: 'quote " and slash \ end') = '"quote \" and slash \\ end"'.
]

"encode an array"
[
	| a |
	a := OrderedCollection new.
	a add: 1; add: 2; add: 3.
	Assert true: (Json encode: a) = '[1,2,3]'.
]

"parse primitives"
[
	Assert true: (Json parse: '42') = 42.
	Assert true: (Json parse: 'true') = true.
	Assert true: (Json parse: 'false') = false.
	Assert true: (Json parse: 'null') isNil.
	Assert true: (Json parse: '  "hello"  ') = 'hello'.
]

"parse an array"
[
	| a |
	a := Json parse: '[1, 2, 3]'.
	Assert true: a size = 3.
	Assert true: (a at: 1) = 1.
	Assert true: (a at: 3) = 3.
]

"parse an object"
[
	| d |
	d := Json parse: '{"name": "Alice", "age": 30, "active": true}'.
	Assert true: (d at: 'name') = 'Alice'.
	Assert true: (d at: 'age') = 30.
	Assert true: (d at: 'active') = true.
]

"parse a nested object with an array of objects"
[
	| d cards |
	d := Json parse: '{"id": 7, "cards": [{"t": "a"}, {"t": "b"}]}'.
	Assert true: (d at: 'id') = 7.
	cards := d at: 'cards'.
	Assert true: cards size = 2.
	Assert true: ((cards at: 1) at: 't') = 'a'.
	Assert true: ((cards at: 2) at: 't') = 'b'.
]

"parse string escapes"
[
	| s |
	s := Json parse: '"a\nb\tc\"d"'.
	Assert true: s size = 7.
	Assert true: (s at: 2) = Character lf.
	Assert true: (s at: 4) = Character tab.
	Assert true: (s at: 6) = $".
]

"round-trip a dictionary through encode then parse"
[
	| d back |
	d := Dictionary new.
	d at: 'title' put: 'Founder'.
	d at: 'views' put: 99.
	d at: 'active' put: true.
	back := Json parse: (Json encode: d).
	Assert true: (back at: 'title') = 'Founder'.
	Assert true: (back at: 'views') = 99.
	Assert true: (back at: 'active') = true.
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
	Assert do: [ Json parse: '' ] expect: JsonParseError.
	Assert do: [ Json parse: '   ' ] expect: JsonParseError.
	Assert do: [ Json parse: 'tru' ] expect: JsonParseError.
	Assert do: [ Json parse: 'truex' ] expect: JsonParseError.
	Assert do: [ Json parse: 'tRuE' ] expect: JsonParseError.
	Assert do: [ Json parse: 'nul' ] expect: JsonParseError.
	Assert do: [ Json parse: '{' ] expect: JsonParseError.
	Assert do: [ Json parse: '[1,' ] expect: JsonParseError.
	Assert do: [ Json parse: '{"a"}' ] expect: JsonParseError.
	Assert do: [ Json parse: '{"a":}' ] expect: JsonParseError.
	Assert do: [ Json parse: '{"a":1,}' ] expect: JsonParseError.
	Assert do: [ Json parse: '{"a":1 "b":2}' ] expect: JsonParseError.
	Assert do: [ Json parse: '{1:2}' ] expect: JsonParseError.
	Assert do: [ Json parse: '[1 2]' ] expect: JsonParseError.
	Assert do: [ Json parse: '}' ] expect: JsonParseError.
	Assert do: [ Json parse: 'null null' ] expect: JsonParseError.
	Assert do: [ Json parse: '{"a":1} x' ] expect: JsonParseError.
	Assert do: [ Json parse: '"aberta' ] expect: JsonParseError.
	Assert do: [ Json parse: '"\q"' ] expect: JsonParseError.
]

"parse error carries the 1-based position"
[
	[ Json parse: '[1,]'. Assert true: false ]
		on: JsonParseError
		do: [:e | Assert true: e position = 4 ].
	[ Json parse: 'nulx'. Assert true: false ]
		on: JsonParseError
		do: [:e | Assert true: e position = 4 ].
]

"raw control characters inside strings are rejected"
[
	| s |
	s := String new: 3.
	s at: 1 put: $".
	s at: 2 put: (Character codePoint: 7).
	s at: 3 put: $".
	Assert do: [ Json parse: s ] expect: JsonParseError.
]

"unicode escapes: lone/bad surrogates and bad hex are rejected"
[
	Assert do: [ Json parse: '"\uD800"' ] expect: JsonParseError.
	Assert do: [ Json parse: '"\uD800\uD800"' ] expect: JsonParseError.
	Assert do: [ Json parse: '"\uDC00"' ] expect: JsonParseError.
	Assert do: [ Json parse: '"\u12G4"' ] expect: JsonParseError.
	Assert do: [ Json parse: '"\u12"' ] expect: JsonParseError.
]

"nesting deeper than maxDepth is rejected (not a hang, not a crash)"
[
	| s |
	s := String new: 200.
	1 to: 200 do: [:i | s at: i put: $[ ].
	Assert do: [ Json parse: s ] expect: JsonParseError.
]

"invalid numbers are rejected"
[
	Assert do: [ Json parse: '01' ] expect: JsonParseError.
	Assert do: [ Json parse: '+1' ] expect: JsonParseError.
	Assert do: [ Json parse: '.5' ] expect: JsonParseError.
	Assert do: [ Json parse: '1.' ] expect: JsonParseError.
	Assert do: [ Json parse: '1e' ] expect: JsonParseError.
	Assert do: [ Json parse: '1e+' ] expect: JsonParseError.
	Assert do: [ Json parse: '-' ] expect: JsonParseError.
	Assert do: [ Json parse: '- 1' ] expect: JsonParseError.
]

"valid numbers: zero forms, exponents, floats"
[
	Assert true: (Json parse: '-0') = 0.
	Assert true: (Json parse: '0') = 0.
	Assert true: (Json parse: '0.5') = 0.5.
	Assert true: (Json parse: '-2.75') = 2.75 negated.
	Assert true: (Json parse: '1e2') = 100.
	Assert true: (Json parse: '1E-2') = 0.01.
	Assert true: (Json parse: '3e+2') = 300.
]

"float round-trips through encode then parse"
[
	Assert true: (Json parse: (Json encode: 0.1)) = 0.1.
	Assert true: (Json parse: (Json encode: 1.5)) = 1.5.
	Assert true: (Json parse: (Json encode: 1.0e300)) = 1.0e300.
	Assert true: (Json parse: (Json encode: 0.1 + 0.2)) = (0.1 + 0.2).
]

"big integers promote to LargeInteger and round-trip"
[
	| big back |
	big := Json parse: '123456789012345678901234567890'.
	Assert true: big printString = '123456789012345678901234567890'.
	back := Json parse: (Json encode: big).
	Assert true: back = big.
	Assert true: (Json parse: '-123456789012345678901234567890') = big negated.
]

"unicode escapes decode to UTF-8 bytes (surrogate pairs included)"
[
	| s |
	s := Json parse: '"\u0041"'.
	Assert true: s = 'A'.
	s := Json parse: '"\u00E9"'.
	Assert true: s size = 2.
	Assert true: (s at: 1) codePoint = 195.
	Assert true: (s at: 2) codePoint = 169.
	s := Json parse: '"\u20AC"'.
	Assert true: s size = 3.
	Assert true: (s at: 1) codePoint = 226.
	s := Json parse: '"\uD83D\uDE00"'.
	Assert true: s size = 4.
	Assert true: (s at: 1) codePoint = 240.
	Assert true: (s at: 2) codePoint = 159.
	Assert true: (s at: 3) codePoint = 152.
	Assert true: (s at: 4) codePoint = 128.
	s := Json parse: '"\u0000"'.
	Assert true: s size = 1.
	Assert true: (s at: 1) codePoint = 0.
	Assert true: (Json parse: '"\/"') = '/'.
]

"encoder: NaN and Infinity are errors; Fraction converts to float"
[
	Assert do: [ Json encode: Float nan ] expect: JsonError.
	Assert do: [ Json encode: Float infinity ] expect: JsonError.
	Assert do: [ Json encode: Float infinity negated ] expect: JsonError.
	Assert true: (Json encode: 1 / 2) = '0.5'.
	Assert true: (Json encode: 3 / 4) = '0.75'.
]

"encoder: symbols, characters, non-string dictionary keys"
[
	| d |
	Assert true: (Json encode: #hello) = '"hello"'.
	Assert true: (Json encode: $a) = '"a"'.
	d := Dictionary new.
	d at: 1 put: 'one'.
	Assert true: (Json encode: d) = '{"1":"one"}'.
]

"encoder: a cyclic structure fails with JsonError instead of hanging"
[
	| a |
	a := OrderedCollection new.
	a add: a.
	Assert do: [ Json encode: a ] expect: JsonError.
]

"decode: is an alias of parse:"
[
	Assert true: (Json decode: '42') = 42.
	Assert true: ((Json decode: '{"k":1}') at: 'k') = ((Json parse: '{"k":1}') at: 'k').
]

"reflective encode of a plain object: one key per instance variable"
[
	| p json back |
	p := JsonTestAddress new.
	p setStreet: 'Rua A' city: 'SP'.
	json := Json encode: p.
	back := Json parse: json.
	Assert true: (back at: 'street') = 'Rua A'.
	Assert true: (back at: 'city') = 'SP'.
]

"jsonOn: hook takes full control of an object's JSON shape"
[
	| c |
	c := JsonTestCustom new.
	c setValue: 42.
	Assert true: (Json encode: c) = '"custom!"'.
]

"typed decode: flat object"
[
	| p |
	p := Json decode: '{"name":"Alice","age":30}' as: JsonTestAddress.
	"keys that do not match ivars are ignored; unmatched ivars stay nil"
	Assert true: p street isNil.
	Assert true: p city isNil.
	p := Json decode: '{"street":"Rua B","city":"Rio"}' as: JsonTestAddress.
	Assert true: p street = 'Rua B'.
	Assert true: p city = 'Rio'.
]

"typed decode: nested objects and collections via jsonTypes"
[
	| p |
	p := Json decode: '{"name":"Bob","age":41,"address":{"street":"Rua C","city":"BH"},"tags":[{"label":"a"},{"label":"b"}]}'
		as: JsonTestPerson.
	Assert true: p name = 'Bob'.
	Assert true: p age = 41.
	Assert true: (p address isKindOf: JsonTestAddress).
	Assert true: p address city = 'BH'.
	Assert true: p tags size = 2.
	Assert true: ((p tags at: 1) isKindOf: JsonTestTag).
	Assert true: (p tags at: 2) label = 'b'.
]

"typed decode: null -> nil, missing -> nil, extra keys ignored"
[
	| p |
	p := Json decode: '{"name":null,"age":7,"extra":true}' as: JsonTestPerson.
	Assert true: p name isNil.
	Assert true: p age = 7.
	Assert true: p address isNil.
	Assert true: p tags isNil.
]

"typed decode of arrays: decode:asArrayOf:"
[
	| list |
	list := Json decode: '[{"label":"x"},{"label":"y"}]' asArrayOf: JsonTestTag.
	Assert true: list size = 2.
	Assert true: (list at: 1) label = 'x'.
	Assert true: (list at: 2) label = 'y'.
]

"object -> JSON -> object round-trip with nesting"
[
	| p json back |
	p := JsonTestPerson new.
	p setName: 'Eva' age: 28.
	p setAddress: (JsonTestAddress new setStreet: 'Rua D' city: 'POA'; yourself).
	p setTags: (Array with: (JsonTestTag new setLabel: 'k'; yourself)).
	json := Json encode: p.
	back := Json decode: json as: JsonTestPerson.
	Assert true: back name = 'Eva'.
	Assert true: back address street = 'Rua D'.
	Assert true: (back tags at: 1) label = 'k'.
]

"whitespace tolerance and BOM rejection"
[
	| bom |
	Assert true: (Json parse: '  [ 1 , 2 ]  ') size = 2.
	bom := String new: 7.
	bom at: 1 put: (Character codePoint: 239).
	bom at: 2 put: (Character codePoint: 187).
	bom at: 3 put: (Character codePoint: 191).
	bom at: 4 put: $t. bom at: 5 put: $r. bom at: 6 put: $u. bom at: 7 put: $e.
	Assert do: [ Json parse: bom ] expect: JsonParseError.
]

"C parser and Smalltalk reference parser agree (structural deep equality —
 dictionary ITERATION order may differ between the two, JSON objects are
 unordered)"
[
	| docs deepEq |
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
		Assert true: (deepEq value: (Json parse: doc) value: (Json new slowParse: doc)) ].
]

"GC stress: large array, large string, many keys (scavenges mid-parse)"
[
	| out big parsed s dict i |
	out := CollectionStream with: (String new: 64).
	out nextPut: $[.
	1 to: 10000 do: [:k |
		k > 1 ifTrue: [ out nextPut: $, ].
		out nextPutAll: k printString ].
	out nextPut: $].
	parsed := Json parse: out contents.
	Assert true: parsed size = 10000.
	Assert true: (parsed at: 1) = 1.
	Assert true: (parsed at: 10000) = 10000.

	s := 'abcdefghijklmnop'.
	1 to: 16 do: [:k | s := s , s ].               "16 doublings -> 1MB"
	big := Json parse: '"' , s , '"'.
	Assert true: big size = s size.
	Assert true: (Json parse: (Json encode: big)) size = s size.

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
	Assert true: i = 5000.
	Assert true: (dict at: 'k1') = 1.
	Assert true: (dict at: 'k5000') = 5000.
]
