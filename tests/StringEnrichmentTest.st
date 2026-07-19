"StringEnrichmentTest: the wave 3 String / Symbol protocol added on top of the
 collection wave. Ordering (compare:/</<=/>/>=), sameAs:, includesSubstring:,
 occurrencesOfString:, indexOf:startingAt:, format: and its % alias,
 replaceAll:with:, the pad family, substrings/substrings:/lines, join:, the
 trims, capitalized, repeat:, asByteArray, asInteger, asDateTime, and the
 Symbol selector-reflection helpers."

[
	| t crlf |
	t := TestRun named: 'StringEnrichment'.
	crlf := (String with: Character cr) , (String with: Character lf).

	"---- ordering ----"
	t check: 'equal strings compare 0' equals: 0 is: [ 'abc' compare: 'abc' ].
	t check: 'shorter prefix compares negative' equals: -1 is: [ 'ab' compare: 'abc' ].
	t check: 'longer compares positive' equals: 1 is: [ 'abc' compare: 'ab' ].
	t check: 'empty compares negative to non-empty' equals: -1 is: [ '' compare: 'a' ].
	t check: 'two empties compare 0' equals: 0 is: [ '' compare: '' ].
	t check: 'single char less' that: [ 'a' < 'b' ].
	t check: 'prefix is less than longer' that: [ 'ab' < 'abc' ].
	t check: 'lowercase sorts after uppercase (case sensitive)' that: [ 'A' < 'a' ].
	t check: '<= reflexive' that: [ 'abc' <= 'abc' ].
	t check: '> works' that: [ 'abd' > 'abc' ].
	t check: '>= reflexive' that: [ 'abc' >= 'abc' ].
	t check: 'not less than equal' that: [ ('abc' < 'abc') not ].
	t check: 'sort of strings uses <=' equals: (Array with: 'apple' with: 'banana' with: 'cherry') is: [
		(Array with: 'cherry' with: 'apple' with: 'banana') sorted ].

	"---- sameAs: ----"
	t check: 'sameAs case-insensitive equal' that: [ 'Hello' sameAs: 'hELLO' ].
	t check: 'sameAs different is false' that: [ ('Hello' sameAs: 'Hallo') not ].
	t check: 'sameAs different length is false' that: [ ('Hello' sameAs: 'Hell') not ].

	"---- includesSubstring: ----"
	t check: 'includesSubstring present' that: [ 'abcdef' includesSubstring: 'cd' ].
	t check: 'includesSubstring at start' that: [ 'abcdef' includesSubstring: 'ab' ].
	t check: 'includesSubstring at end' that: [ 'abcdef' includesSubstring: 'ef' ].
	t check: 'includesSubstring absent' that: [ ('abcdef' includesSubstring: 'xy') not ].
	t check: 'includesSubstring empty is present' that: [ 'abc' includesSubstring: '' ].

	"---- indexOf:startingAt: convenience ----"
	t check: 'indexOf from start' equals: 2 is: [ 'banana' indexOf: $a startingAt: 1 ].
	t check: 'indexOf resumes past first' equals: 4 is: [ 'banana' indexOf: $a startingAt: 3 ].
	t check: 'indexOf absent is 0' equals: 0 is: [ 'banana' indexOf: $z startingAt: 1 ].

	"---- occurrencesOfString: (non-overlapping) ----"
	t check: 'occurrences non-overlapping aa in aaaa' equals: 2 is: [ 'aaaa' occurrencesOfString: 'aa' ].
	t check: 'occurrences ab in ababab' equals: 3 is: [ 'ababab' occurrencesOfString: 'ab' ].
	t check: 'occurrences absent is 0' equals: 0 is: [ 'abcd' occurrencesOfString: 'xy' ].
	t check: 'occurrences of empty is 0' equals: 0 is: [ 'abcd' occurrencesOfString: '' ].

	"---- format: and % ----"
	t check: 'format single placeholder' equals: 'hi x' is: [ 'hi {1}' format: (Array with: 'x') ].
	t check: 'format multiple placeholders' equals: 'x-y' is: [
		'{1}-{2}' format: (Array with: 'x' with: 'y') ].
	t check: 'format repeated placeholder' equals: 'xx' is: [ '{1}{1}' format: (Array with: 'x') ].
	t check: 'format uses displayString (no quotes)' equals: 'n=42' is: [
		'n={1}' format: (Array with: 42) ].
	t check: 'format lone brace is literal' equals: 'a{b' is: [ 'a{b' format: (Array with: 'z') ].
	t check: 'percent alias of format' equals: '[7]' is: [ '[{1}]' % (Array with: 7) ].

	"---- replaceAll:with: (in place, returns self) ----"
	t check: 'replaceAll swaps chars' equals: 'a_b_c' is: [ ('a-b-c' copy) replaceAll: $- with: $_ ].
	t check: 'replaceAll returns the receiver' that: [ | s | s := 'aaa' copy. (s replaceAll: $a with: $b) == s ].
	t check: 'replaceAll mutates in place' equals: 'bbb' is: [ | s | s := 'aaa' copy. s replaceAll: $a with: $b. s ].

	"---- pads ----"
	t check: 'padLeftTo with char' equals: '007' is: [ '7' padLeftTo: 3 with: $0 ].
	t check: 'padLeftTo default space' equals: '  7' is: [ '7' padLeftTo: 3 ].
	t check: 'padRightTo with char' equals: '7..' is: [ '7' padRightTo: 3 with: $. ].
	t check: 'padRightTo default space' equals: '7  ' is: [ '7' padRightTo: 3 ].
	t check: 'pad already bigger is untouched' equals: 'abcd' is: [ 'abcd' padLeftTo: 2 with: $0 ].
	t check: 'pad exactly width is untouched' equals: 'abc' is: [ 'abc' padLeftTo: 3 with: $0 ].

	"---- substrings / substrings: / lines ----"
	t check: 'substrings collapses whitespace' equals: (Array with: 'a' with: 'b' with: 'c') is: [
		'  a  b   c ' substrings asArray ].
	t check: 'substrings of all spaces is empty' equals: (Array new: 0) is: [ '    ' substrings asArray ].
	t check: 'substrings single token' equals: (Array with: 'word') is: [ 'word' substrings asArray ].
	t check: 'substrings: by set of chars' equals: (Array with: 'a' with: 'b' with: 'c') is: [
		('a,b;c' substrings: ',;') asArray ].
	t check: 'lines with LF' equals: (Array with: 'a' with: 'b' with: 'c') is: [
		('a' , (String with: Character lf) , 'b' , (String with: Character lf) , 'c') lines asArray ].
	t check: 'lines with CRLF' equals: (Array with: 'a' with: 'b') is: [
		('a' , crlf , 'b') lines asArray ].
	t check: 'lines trailing LF has no empty final line' equals: (Array with: 'a' with: 'b') is: [
		('a' , (String with: Character lf) , 'b' , (String with: Character lf)) lines asArray ].
	t check: 'lines with blank line preserved' equals: (Array with: 'a' with: '' with: 'b') is: [
		('a' , (String with: Character lf) , (String with: Character lf) , 'b') lines asArray ].

	"---- join: ----"
	t check: 'join empty collection' equals: '' is: [ ', ' join: (Array new: 0) ].
	t check: 'join single element' equals: 'a' is: [ ', ' join: (Array with: 'a') ].
	t check: 'join many elements' equals: 'a, b, c' is: [ ', ' join: (Array with: 'a' with: 'b' with: 'c') ].
	t check: 'join uses displayString of elements' equals: '1-2-3' is: [
		'-' join: (Array with: 1 with: 2 with: 3) ].

	"---- trims and capitalized ----"
	t check: 'trimLeft' equals: 'hi ' is: [ '  hi ' trimLeft ].
	t check: 'trimRight' equals: '  hi' is: [ '  hi ' trimRight ].
	t check: 'trim both ends' equals: 'hi' is: [ '  hi  ' trim ].
	t check: 'trim of all spaces is empty' equals: '' is: [ '    ' trim ].
	t check: 'capitalized' equals: 'Hello' is: [ 'hello' capitalized ].
	t check: 'capitalized leaves rest untouched' equals: 'HeLLo' is: [ 'heLLo' capitalized ].
	t check: 'capitalized empty' equals: '' is: [ '' capitalized ].
	t check: 'asLowercaseFirst' equals: 'hELLO' is: [ 'HELLO' asLowercaseFirst ].

	"---- repeat: / asByteArray / asInteger ----"
	t check: 'repeat n times' equals: 'ababab' is: [ 'ab' repeat: 3 ].
	t check: 'repeat zero is empty' equals: '' is: [ 'ab' repeat: 0 ].
	t check: 'asByteArray raw code points' equals: (Array with: 65 with: 66 with: 67) is: [
		'ABC' asByteArray asArray ].
	t check: 'asInteger of digits' equals: 42 is: [ '42' asInteger ].
	t check: 'asInteger negative' equals: -5 is: [ '-5' asInteger ].
	t check: 'asInteger trims whitespace' equals: 7 is: [ '  7  ' asInteger ].
	t check: 'asInteger of garbage is nil' that: [ '4x2' asInteger isNil ].
	t check: 'asInteger of empty is nil' that: [ '' asInteger isNil ].
	t check: 'asInteger of pure letters is nil' that: [ 'abc' asInteger isNil ].

	"---- asDateTime roundtrip ----"
	t check: 'asDateTime roundtrips through printIso8601' equals: '2026-07-19T12:30:00Z' is: [
		'2026-07-19T12:30:00Z' asDateTime printIso8601 ].
	t check: 'asDate from ISO string' equals: 2026 is: [ '2026-07-19' asDate year ].

	"---- Symbol selector reflection ----"
	t check: 'numArgs of unary #foo' equals: 0 is: [ #foo numArgs ].
	t check: 'numArgs of binary #+' equals: 1 is: [ #+ numArgs ].
	t check: 'numArgs of keyword #at:put:' equals: 2 is: [ #at:put: numArgs ].
	t check: 'isUnary #size' that: [ #size isUnary ].
	t check: 'isBinary #+' that: [ #+ isBinary ].
	t check: 'isBinary #<= (multi-char)' that: [ #<= isBinary ].
	t check: 'isKeywordSelector #at:put:' that: [ #at:put: isKeywordSelector ].
	t check: 'unary is not keyword selector' that: [ #foo isKeywordSelector not ].
	t check: 'binary is not unary' that: [ #+ isUnary not ].

	^t report
]
