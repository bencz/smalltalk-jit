Character := Magnitude [

	| Table |


	"instance creation"

	class initialize [
		Table := ByteArray new: 256 * 4.

		0 to: 255 do: [ :i |
			(i between: 48 and: 57) "0-9"
				ifTrue: [
					Table at: i + 1 put: i - 48.
					Table at: i + 257 put: 253 "digit"]
				ifFalse: [Table at: i + 1 put: 36].
			(i between: 65 and: 90) "A-B"
				ifTrue: [
					Table at: i + 1 put: i - 55. "digit value"
					Table at: i + 257 put: 254. "letter"
					Table at: i + 513 put: i + 32 "lower"]
				ifFalse: [
					Table at: i + 513 put: i "lower"].
			(i between: 97 and: 122) "a-b"
				ifTrue: [
					Table at: i + 257 put: 254. "letter"
					Table at: i + 769 put: i - 32 "upper"]
				ifFalse: [
					Table at: i + 769 put: i "upper"].
		].

		Table
			at: $+ codePoint + 257 put: 252;
			at: $- codePoint + 257 put: 252;
			at: $/ codePoint + 257 put: 252;
			at: $\ codePoint + 257 put: 252;
			at: $* codePoint + 257 put: 252;
			at: $~ codePoint + 257 put: 252;
			at: $< codePoint + 257 put: 252;
			at: $> codePoint + 257 put: 252;
			at: $= codePoint + 257 put: 252;
			at: $@ codePoint + 257 put: 252;
			at: $% codePoint + 257 put: 252;
			at: $| codePoint + 257 put: 252;
			at: $& codePoint + 257 put: 252;
			at: $? codePoint + 257 put: 252;
			at: $! codePoint + 257 put: 252;
			","

			at: $" codePoint + 257 put: 251;
			at: $# codePoint + 257 put: 251;
			at: $$ codePoint + 257 put: 251;
			at: $' codePoint + 257 put: 251;
			at: $( codePoint + 257 put: 251;
			at: $) codePoint + 257 put: 251;
			at: $; codePoint + 257 put: 251;
			at: $[ codePoint + 257 put: 251;
			at: $] codePoint + 257 put: 251;
			at: $^ codePoint + 257 put: 251;
			at: $_ codePoint + 257 put: 251;
			at: $` codePoint + 257 put: 251;
			at: ${ codePoint + 257 put: 251;
			at: $} codePoint + 257 put: 251;

			at: 9 + 257 put: 250; "tab"
			at: 10 + 257 put: 250; "lf"
			at: 12 + 257 put: 250; "ff"
			at: 13 + 257 put: 250; "cr"
			at: 32 + 257 put: 250; "space"

			at: $A codePoint + 257 put: 255;
			at: $E codePoint + 257 put: 255;
			at: $I codePoint + 257 put: 255;
			at: $O codePoint + 257 put: 255;
			at: $U codePoint + 257 put: 255;
			at: $a codePoint + 257 put: 255;
			at: $e codePoint + 257 put: 255;
			at: $i codePoint + 257 put: 255;
			at: $o codePoint + 257 put: 255;
			at: $u codePoint + 257 put: 255.
	]


	class new [
		ShouldNotImplement signal.
	]


	class codePoint: anInteger [
		<primitive: CharacterNewPrimitive>
		Error signal.
	]


	class value: anInteger [
		"Alias for codePoint:: answer the Character with the given code point."
		^self codePoint: anInteger
	]


	class digitValue: anInteger [
		^'0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ' at: anInteger + 1
	]


	"untypeable characters "

	class cr [
		^self codePoint: 13
	]


	class lf [
		^self codePoint: 10
	]


	class space [
		 ^self codePoint: 32
	]


	class tab [
		^self codePoint: 9
	]


	"accessing"

	codePoint [
		<primitive: CharacterCodePrimitive>
	]


	digitValue [
		| digitValue |
		digitValue := Table at: self codePoint + 1.
		digitValue = 36 ifTrue: [self error: 'character is not digit'].
		^digitValue
	]


	value [
		"Alias for codePoint."
		^self codePoint
	]


	asInteger [
		"Alias for codePoint."
		^self codePoint
	]


	"comparing"

	hash [
		^0 - self codePoint
	]


	identityHash [
		"Characters are immediates: the header-reading HashPrimitive that
		Object>>identityHash uses would dereference the tagged word."
		^self hash
	]

	< aCharacter [
		^self codePoint < aCharacter codePoint
	]


	sameAs: aCharacter [
		"Case-insensitive comparison (ASCII fold)."
		^self asLowercase = aCharacter asLowercase
	]


	"testing"

	isAlphaNumeric [
		^(Table at: self codePoint + 257) > 252
	]


	isDigit [
		^self isDigit: 10
	]


	isDigit: anInteger [
		^(Table at: self codePoint + 1) < anInteger
	]


	isLetter [
		^(Table at: self codePoint + 257) > 253
	]


	isSeparator [
		^(Table at: self codePoint + 257) = 250
	]


	isSpecial [
		^(Table at: self codePoint + 257) = 252
	]


	isLowercase [
		^self asLowercase = self
	]


	isUppercase [
		^self asUppercase = self
	]


	isVowel [
		^(Table at: self codePoint + 257) = 255
	]


	isPunctuation [
		"True for ASCII punctuation: code points 33..47, 58..64, 91..96, 123..126
		 (the C ispunct ranges)."
		| c |
		c := self codePoint.
		^(c between: 33 and: 47) or: [(c between: 58 and: 64)
			or: [(c between: 91 and: 96) or: [c between: 123 and: 126]]]
	]


	"converting"

	asLowercase [
		^self class codePoint: (Table at: self codePoint + 513)
	]


	asUppercase [
		^self class codePoint: (Table at: self codePoint + 769)
	]


	asCharacter [
		^self
	]


	asString [
		"A one-character String containing the receiver."
		^String with: self
	]


	asSymbol [
		^self asString asSymbol
	]


	to: aCharacter [
		"Answer an Array of the consecutive Characters from the receiver to
		 aCharacter inclusive (empty when aCharacter precedes the receiver)."
		| start stop result |
		start := self codePoint.
		stop := aCharacter codePoint.
		stop < start ifTrue: [^Array new: 0].
		result := Array new: stop - start + 1.
		start to: stop do: [ :c | result at: c - start + 1 put: (Character value: c)].
		^result
	]


	"printing"

	printOn: aStream [
		aStream nextPut: $$.
		(Table at: self codePoint + 257) > 250
			ifTrue: [aStream nextPut: self]
			ifFalse: [
				self = self class space	ifTrue: [
					aStream nextPutAll: '<SP>'.
					^self].
				self = self class lf ifTrue: [
					aStream nextPutAll: '<LF>'.
					^self].
				self = self class cr ifTrue: [
					aStream nextPutAll: '<CR>'.
					^self].
				aStream nextPut: $<.
				self codePoint printOn: aStream.
				aStream nextPut: $>].
	]


	"displaying"

	displayString [
		"The character itself, without the leading $ that printOn: adds."
		^self asString
	]


	displayOn: aStream [
		aStream nextPut: self.
	]

]
