"Regression for the numeric-tower rework: Float>>hash used to RAISE for any
magnitude beyond the SmallInteger range (1.0e300 hash), because truncated had
no big-integer fallback. Uses the fails-counter pattern: a bare Assert failure
still exits 0, only ^failCount is caught by run_tests.sh."

[
	| fails big |
	fails := 0.

	"hash must work across the whole Float range and stay value-consistent"
	(1.0e300 hash isKindOf: Integer) ifFalse: [fails := fails + 1].
	(2.0 hash = 2 hash) ifFalse: [fails := fails + 2].
	(2.0 = 2) ifFalse: [fails := fails + 4].

	"truncated beyond the SmallInteger range builds the EXACT integer"
	big := 1.0e300 truncated.
	(big class == LargeInteger) ifFalse: [fails := fails + 8].
	(big asFloat = 1.0e300) ifFalse: [fails := fails + 16].
	(1.0e300 hash = big hash) ifFalse: [fails := fails + 32].
	((-1.5e300) truncated asFloat = -1.5e300) ifFalse: [fails := fails + 64].

	"floor/ceiling/rounded share the fallback (huge floats are integral)"
	(1.0e300 floor asFloat = 1.0e300) ifFalse: [fails := fails + 128].
	(1.0e300 ceiling asFloat = 1.0e300) ifFalse: [fails := fails + 256].
	(1.0e300 rounded asFloat = 1.0e300) ifFalse: [fails := fails + 512].

	"the in-range prim path is untouched"
	(123456.75 truncated = 123456) ifFalse: [fails := fails + 1024].
	(2.5 hash = 2 hash) ifFalse: [fails := fails + 2048].

	"Inf/NaN: fixed hash, no raise (Float infinity = Float infinity holds)"
	(Float infinity hash = 0) ifFalse: [fails := fails + 4096].
	(Float nan hash = 0) ifFalse: [fails := fails + 8192].

	"the decompose primitives themselves"
	(1.0 exponent = 0) ifFalse: [fails := fails + 16384].
	(0.5 exponent = -1) ifFalse: [fails := fails + 32768].
	((1.0 timesTwoPower: 3) = 8.0) ifFalse: [fails := fails + 65536].
	((3.0 timesTwoPower: -1) = 1.5) ifFalse: [fails := fails + 131072].

	^fails
]
