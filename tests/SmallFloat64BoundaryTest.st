"Boundary tests for the SmallFloat64 immediate representation: which doubles
ride in the tagged word (class SmallFloat64) and which stay heap boxed (class
BoxedFloat64). The bit-exact encode/decode itself is proven by the C-level
ST_SMALLFLOAT_TEST; this covers the Smalltalk-visible class split and the
mixed semantics. Fails-counter pattern (bare Assert exits 0)."

[
	| fails |
	fails := 0.

	"everyday values are immediates"
	(1.0 class == SmallFloat64) ifFalse: [fails := fails + 1].
	(0.0 class == SmallFloat64) ifFalse: [fails := fails + 2].
	((0.0 - 0.0) class == SmallFloat64) ifFalse: [fails := fails + 4].
	((2.0 + 3.5) class == SmallFloat64) ifFalse: [fails := fails + 8].

	"the immediate range is 2^-255 .. just under 2^257 in magnitude"
	((1.0 timesTwoPower: 256) class == SmallFloat64) ifFalse: [fails := fails + 16].
	((1.0 timesTwoPower: 257) class == BoxedFloat64) ifFalse: [fails := fails + 32].
	((1.0 timesTwoPower: -254) class == SmallFloat64) ifFalse: [fails := fails + 64].
	((1.5 timesTwoPower: -255) class == SmallFloat64) ifFalse: [fails := fails + 128].

	"exact +-2^-255 would collide with +-0.0 payloads: they must box"
	((1.0 timesTwoPower: -255) class == BoxedFloat64) ifFalse: [fails := fails + 256].
	((-1.0 timesTwoPower: -255) class == BoxedFloat64) ifFalse: [fails := fails + 512].

	"subnormals, infinities and NaN always box"
	((1.0 timesTwoPower: -1060) class == BoxedFloat64) ifFalse: [fails := fails + 1024].
	(Float infinity class == BoxedFloat64) ifFalse: [fails := fails + 2048].
	(Float nan class == BoxedFloat64) ifFalse: [fails := fails + 4096].

	"negative zero keeps its sign bit through the immediate encoding:
	dividing by it must give the right-signed infinity"
	((1.0 / (0.0 - 0.0)) = Float infinity) ifFalse: [fails := fails + 8192].
	((1.0 / ((-1.0) * 0.0)) = (0.0 - Float infinity)) ifFalse: [fails := fails + 16384].

	"value semantics across the split"
	((1.0 timesTwoPower: 257) = (1.0 timesTwoPower: 257)) ifFalse: [fails := fails + 32768].
	((2.5 + 2.5) == 5.0) ifFalse: [fails := fails + 65536].
	((1.0e300 = 2.0) not) ifFalse: [fails := fails + 131072].

	"arithmetic crossing the boundary boxes the result correctly"
	(((1.0 timesTwoPower: 256) * 4.0) class == BoxedFloat64) ifFalse: [fails := fails + 262144].
	(((1.0 timesTwoPower: 258) / 8.0) class == SmallFloat64) ifFalse: [fails := fails + 524288].

	^fails
]
