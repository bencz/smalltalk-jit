"RFC 4648 base-64 codec using the standard alphabet (A-Za-z0-9+/) and '='
 padding. encode: accepts a String or a ByteArray and answers a padded base-64
 String; decode: accepts a base-64 String and answers the decoded bytes as a
 String (decodeBytes: as a ByteArray), raising InvalidArgumentError on
 malformed input. encodeUrlSafe:/decodeUrlSafe:/decodeBytesUrlSafe: are the
 base64url variant (section 5: -_ alphabet, no padding). The hot paths run in
 C (vm/runtime/Base64.c); the Smalltalk bodies below the pragmas are the
 validating fallback and the reference implementation - keep semantics in
 sync. The alphabet and the reverse lookup table are built once in class
 initialize."

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
		<primitive: Base64EncodePrimitive>
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
		<primitive: Base64DecodePrimitive>
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
		"strict RFC 4648: '=' is only valid as final padding, so everything
		 before the tail must be an alphabet character (valueOf: rejects '=')"
		1 to: n - pad do: [ :i |
			c := aString at: i.
			v := self valueOf: c.
			v < 0 ifTrue: [^self decodeError].
			accum := (accum bitShift: 6) bitOr: v.
			bits := bits + 6.
			bits >= 8 ifTrue: [
				bits := bits - 8.
				out at: oi put: (Character codePoint: ((accum bitShift: bits negated) bitAnd: 255)).
				oi := oi + 1]].
		^out
	]


	class decodeBytes: aString [
		"Like decode: but answers a ByteArray."
		<primitive: Base64DecodeBytesPrimitive>
		| s ba |

		s := self decode: aString.
		ba := ByteArray new: s size.
		1 to: s size do: [ :i | ba at: i put: (s at: i) codePoint].
		^ba
	]


	"--- URL-safe variant (RFC 4648 section 5): -_ alphabet, no padding ---"

	class encodeUrlSafe: aCollection [
		| s n out c |

		s := self encode: aCollection.
		n := s size.
		[n > 0 and: [(s at: n) = $=]] whileTrue: [n := n - 1].
		out := String new: n.
		1 to: n do: [ :i |
			c := s at: i.
			c = $+ ifTrue: [c := $-].
			c = $/ ifTrue: [c := $_].
			out at: i put: c].
		^out
	]


	class decodeUrlSafe: aString [
		^self decode: (self urlSafeToStandard: aString)
	]


	class decodeBytesUrlSafe: aString [
		^self decodeBytes: (self urlSafeToStandard: aString)
	]


	"--- private ---"

	class urlSafeToStandard: aString [
		"Translate base64url back to the standard alphabet and re-pad."
		| n pad out c |

		n := aString size.
		pad := (4 - (n \\ 4)) \\ 4.
		pad = 3 ifTrue: [^self decodeError].
		out := String new: n + pad.
		1 to: n do: [ :i |
			c := aString at: i.
			c = $- ifTrue: [c := $+].
			c = $_ ifTrue: [c := $/].
			out at: i put: c].
		1 to: pad do: [ :i | out at: n + i put: $=].
		^out
	]


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
