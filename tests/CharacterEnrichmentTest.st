"CharacterEnrichmentTest: wave 3 Character protocol. value/asInteger aliases of
 codePoint, class value: alias of codePoint:, asCharacter, asString, asSymbol,
 to: (inclusive Array of Characters), sameAs: (case-insensitive), and
 isPunctuation."

[
	| t range |
	t := TestRun named: 'CharacterEnrichment'.

	"---- value / asInteger / class value: ----"
	t check: 'value is the code point' equals: 65 is: [ $A value ].
	t check: 'asInteger is the code point' equals: 97 is: [ $a asInteger ].
	t check: 'value and codePoint agree' that: [ $Z value = $Z codePoint ].
	t check: 'class value: builds the character' equals: $B is: [ Character value: 66 ].
	t check: 'class value: is codePoint: alias' that: [ (Character value: 48) = (Character codePoint: 48) ].

	"---- asCharacter / asString / asSymbol ----"
	t check: 'asCharacter answers self' that: [ $x asCharacter == $x ].
	t check: 'asString is a one-char String' equals: 'A' is: [ $A asString ].
	t check: 'asString class is String' that: [ $A asString class == String ].
	t check: 'asString size is 1' equals: 1 is: [ $A asString size ].
	t check: 'asSymbol' equals: #A is: [ $A asSymbol ].

	"---- to: (inclusive) ----"
	range := $a to: $e.
	t check: '$a to: $e has 5 elements (inclusive)' equals: 5 is: [ range size ].
	t check: '$a to: $e first is $a' equals: $a is: [ range first ].
	t check: '$a to: $e last is $e' equals: $e is: [ range last ].
	t check: '$a to: $e is an Array' that: [ range class == Array ].
	t check: '$a to: $e as string' equals: 'abcde' is: [ String withAll: ($a to: $e) ].
	t check: 'descending range is empty' equals: 0 is: [ ($e to: $a) size ].
	t check: 'single-char range' equals: 1 is: [ ($a to: $a) size ].

	"---- sameAs: (case-insensitive) ----"
	t check: 'sameAs uppercase vs lowercase' that: [ $A sameAs: $a ].
	t check: 'sameAs identical' that: [ $x sameAs: $x ].
	t check: 'sameAs different letters is false' that: [ ($a sameAs: $b) not ].

	"---- isPunctuation ----"
	t check: 'comma is punctuation' that: [ $, isPunctuation ].
	t check: 'letter is not punctuation' that: [ $a isPunctuation not ].
	t check: 'digit is not punctuation' that: [ $5 isPunctuation not ].
	t check: 'space is not punctuation' that: [ Character space isPunctuation not ].

	^t report
]
