"RFC 4122 version-4 (random) UUID generation. Answers the canonical 36-char
 lowercase hyphenated form, e.g. '5f5c8d0e-6b3a-4a1e-89bf-1c2d3e4f5a6b'. The 16
 random bytes come from System randomBytes:; the hex is assembled from a private
 nibble table (never printStringBase:) so a byte is always two lowercase hex
 digits."

Uuid := Object [

	class generate [
		| bytes hex out pos b |

		bytes := System randomBytes: 16.
		"Byte 7 high nibble carries the version (4); byte 9 top bits carry the
		 RFC 4122 variant (binary 10xx)."
		bytes at: 7 put: (((bytes at: 7) bitAnd: 15) bitOr: 64).
		bytes at: 9 put: (((bytes at: 9) bitAnd: 63) bitOr: 128).
		hex := '0123456789abcdef'.
		out := String new: 36.
		pos := 1.
		1 to: 16 do: [ :i |
			(i = 5 or: [i = 7 or: [i = 9 or: [i = 11]]]) ifTrue: [
				out at: pos put: $-.
				pos := pos + 1].
			b := bytes at: i.
			out at: pos put: (hex at: (b // 16) + 1).
			out at: pos + 1 put: (hex at: (b \\ 16) + 1).
			pos := pos + 2].
		^out
	]

]
