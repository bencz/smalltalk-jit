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
