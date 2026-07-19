"RFC 4648 base-64 codec using the standard alphabet (A-Za-z0-9+/) and '='
 padding. encode: accepts a String or a ByteArray and answers a padded base-64
 String; decode: accepts a base-64 String and answers the decoded bytes as a
 String, raising InvalidArgumentError on malformed input. The alphabet and the
 reverse lookup table are built once in class initialize."

Base64 := Object [

	| Alphabet DecodeTable |


	class initialize [
		Alphabet := 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'.
		"Reverse map, indexed by codePoint+1 over the 7-bit ASCII range. Entries
		 left nil mean 'not a base-64 character'."
		DecodeTable := Array new: 128.
		1 to: Alphabet size do: [ :j |
			DecodeTable at: (Alphabet at: j) codePoint + 1 put: j - 1].
	]


	"--- encoding ---"

	class encode: aCollection [
		| bytes n out oi bi b0 b1 b2 |

		bytes := self bytesOf: aCollection.
		n := bytes size.
		out := String new: ((n + 2) // 3) * 4.
		bi := 1.
		oi := 1.
		[bi <= n] whileTrue: [
			b0 := bytes at: bi.
			out at: oi put: (Alphabet at: (b0 bitShift: -2) + 1).
			(bi + 1 <= n)
				ifTrue: [
					b1 := bytes at: bi + 1.
					out at: oi + 1 put: (Alphabet at: (((b0 bitAnd: 3) bitShift: 4) bitOr: (b1 bitShift: -4)) + 1).
					(bi + 2 <= n)
						ifTrue: [
							b2 := bytes at: bi + 2.
							out at: oi + 2 put: (Alphabet at: (((b1 bitAnd: 15) bitShift: 2) bitOr: (b2 bitShift: -6)) + 1).
							out at: oi + 3 put: (Alphabet at: (b2 bitAnd: 63) + 1)]
						ifFalse: [
							out at: oi + 2 put: (Alphabet at: ((b1 bitAnd: 15) bitShift: 2) + 1).
							out at: oi + 3 put: $=]]
				ifFalse: [
					out at: oi + 1 put: (Alphabet at: ((b0 bitAnd: 3) bitShift: 4) + 1).
					out at: oi + 2 put: $=.
					out at: oi + 3 put: $=].
			bi := bi + 3.
			oi := oi + 4].
		^out
	]


	"--- decoding ---"

	class decode: aString [
		| n out oi accum bits pad v c |

		n := aString size.
		n = 0 ifTrue: [^''].
		(n \\ 4) = 0 ifFalse: [^self decodeError].
		pad := 0.
		(aString at: n) = $= ifTrue: [
			pad := 1.
			(aString at: n - 1) = $= ifTrue: [pad := 2]].
		out := String new: (n // 4 * 3) - pad.
		oi := 1.
		accum := 0.
		bits := 0.
		1 to: n do: [ :i |
			c := aString at: i.
			c = $= ifFalse: [
				v := self valueOf: c.
				v < 0 ifTrue: [^self decodeError].
				accum := (accum bitShift: 6) bitOr: v.
				bits := bits + 6.
				bits >= 8 ifTrue: [
					bits := bits - 8.
					out at: oi put: (Character codePoint: ((accum bitShift: bits negated) bitAnd: 255)).
					oi := oi + 1]]].
		^out
	]


	"--- private ---"

	class bytesOf: aCollection [
		"Answer aCollection's bytes as a ByteArray. A ByteArray is used as-is; a
		 String is read byte by byte via codePoint. Anything else is rejected."
		| ba |

		(aCollection isKindOf: ByteArray) ifTrue: [^aCollection].
		(aCollection isKindOf: String) ifFalse: [
			^InvalidArgumentError signal: 'Base64 encode: expects a String or ByteArray'].
		ba := ByteArray new: aCollection size.
		1 to: aCollection size do: [ :i | ba at: i put: (aCollection at: i) codePoint].
		^ba
	]


	class valueOf: aCharacter [
		"The 0..63 value of a base-64 character, or -1 when it is not one."
		| cp entry |

		cp := aCharacter codePoint.
		cp >= 128 ifTrue: [^-1].
		entry := DecodeTable at: cp + 1.
		^entry isNil ifTrue: [-1] ifFalse: [entry]
	]


	class decodeError [
		^InvalidArgumentError signal: 'invalid base-64 input'
	]

]
