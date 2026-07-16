"Exhaustive Float edge-case battery: overflow, underflow and subnormals, NaN,
infinities, signed zero, rounding and truncation, precision, cross-type
coercion, printing and exceptions. Each section is a class-side method that
prints one line per failure and returns its failure count; the final block
answers the total, which is the only thing run_tests.sh can catch (a bare
Assert failure still exits 0). IEEE-754 references: doubles have a 53-bit
significand, max finite 1.7976931348623157e308, min normal
2.2250738585072014e-308, min subnormal 5e-324; the VM keeps full IEEE
semantics, only the REPRESENTATION (SmallFloat64 immediate vs BoxedFloat64
heap box) varies with magnitude."

FloatEdgeTest := Object [

	class check: name is: ok [
		ok ifTrue: [^0].
		Transcript nextPutAll: 'FAIL: ', name; lf.
		^1
	]


	class maxDouble [
		^1.7976931348623157e308
	]


	class negativeZero [
		"0.0 negated is +0.0 (it is 0.0 - self); a true -0.0 needs the product"
		^(-1.0) * 0.0
	]


	class overflow [
		| f |
		f := 0.
		f := f + (self check: 'max double is finite' is: self maxDouble isFinite).
		f := f + (self check: 'max double stays boxed' is: self maxDouble class == BoxedFloat64).
		f := f + (self check: 'max * 2 overflows to inf' is: (self maxDouble * 2.0) = Float infinity).
		f := f + (self check: 'max + max overflows to inf' is: (self maxDouble + self maxDouble) = Float infinity).
		f := f + (self check: 'negative overflow to -inf' is: ((0.0 - self maxDouble) * 2.0) = Float infinity negated).
		f := f + (self check: 'exp overflow' is: 1000.0 exp = Float infinity).
		f := f + (self check: 'overflow result boxes' is: (self maxDouble * 2.0) class == BoxedFloat64).
		f := f + (self check: 'near-max arithmetic still finite' is: (self maxDouble / 2.0 * 2.0) = self maxDouble).
		^f
	]


	class underflow [
		| f minNormal minSub |
		f := 0.
		minNormal := 2.2250738585072014e-308.
		minSub := 5e-324.
		f := f + (self check: 'min normal is positive' is: minNormal > 0.0).
		f := f + (self check: 'min normal boxes (below 2^-255)' is: minNormal class == BoxedFloat64).
		f := f + (self check: 'min subnormal is positive' is: minSub > 0.0).
		f := f + (self check: 'min subnormal boxes' is: minSub class == BoxedFloat64).
		f := f + (self check: 'subnormal / 2 underflows to zero' is: (minSub / 2.0) = 0.0).
		f := f + (self check: 'gradual underflow keeps a value' is: (minNormal / 2.0) > 0.0).
		f := f + (self check: 'gradual underflow loses precision' is: (minNormal / 2.0) < minNormal).
		f := f + (self check: 'exp underflow to zero' is: (0.0 - 1000.0) exp = 0.0).
		f := f + (self check: 'tiny * tiny underflows to zero' is: (1.0e-200 * 1.0e-200) = 0.0).
		f := f + (self check: 'underflowed zero is immediate' is: (1.0e-200 * 1.0e-200) class == SmallFloat64).
		^f
	]


	class nan [
		| f nan |
		f := 0.
		nan := Float nan.
		f := f + (self check: '0/0 is NaN' is: (0.0 / 0.0) isNaN).
		f := f + (self check: 'inf - inf is NaN' is: (Float infinity - Float infinity) isNaN).
		f := f + (self check: 'inf * 0 is NaN' is: (Float infinity * 0.0) isNaN).
		f := f + (self check: 'inf / inf is NaN' is: (Float infinity / Float infinity) isNaN).
		f := f + (self check: 'sqrt of negative is NaN' is: (0.0 - 1.0) sqrt isNaN).
		f := f + (self check: 'ln of negative is NaN' is: (0.0 - 1.0) ln isNaN).
		f := f + (self check: 'arcCos out of domain is NaN' is: 2.0 arcCos isNaN).
		f := f + (self check: 'NaN + 1 propagates' is: (nan + 1.0) isNaN).
		f := f + (self check: 'NaN - 1 propagates' is: (nan - 1.0) isNaN).
		f := f + (self check: 'NaN * 2 propagates' is: (nan * 2.0) isNaN).
		f := f + (self check: 'NaN / 2 propagates' is: (nan / 2.0) isNaN).
		f := f + (self check: '1 + NaN propagates' is: (1.0 + nan) isNaN).
		f := f + (self check: 'NaN = NaN is false' is: (nan = nan) not).
		f := f + (self check: 'NaN ~= NaN is true' is: nan ~= nan).
		f := f + (self check: 'NaN < 1 is false' is: (nan < 1.0) not).
		f := f + (self check: 'NaN > 1 is false' is: (nan > 1.0) not).
		f := f + (self check: 'NaN <= 1 is false' is: (nan <= 1.0) not).
		f := f + (self check: 'NaN >= 1 is false' is: (nan >= 1.0) not).
		f := f + (self check: '1 < NaN is false' is: (1.0 < nan) not).
		f := f + (self check: 'NaN = integer is false' is: (nan = 1) not).

		"the same orderings through the DISPATCH path (mixed int argument
		misses the JIT intrinsic): Magnitude's derived >= and <= would
		wrongly answer true here without Float's own overrides"
		f := f + (self check: 'NaN < mixed int is false' is: (nan < 1) not).
		f := f + (self check: 'NaN > mixed int is false' is: (nan > 1) not).
		f := f + (self check: 'NaN <= mixed int is false' is: (nan <= 1) not).
		f := f + (self check: 'NaN >= mixed int is false' is: (nan >= 1) not).
		f := f + (self check: 'mixed int ordering still works' is: (1 < 2.5) & (3 > 2.5) & (2 <= 2.0) & (2 >= 2.0)).
		f := f + (self check: 'NaN isNaN' is: nan isNaN).
		f := f + (self check: 'NaN is not finite' is: nan isFinite not).
		f := f + (self check: 'NaN is not infinite' is: nan isInfinite not).
		f := f + (self check: 'NaN boxes' is: nan class == BoxedFloat64).
		f := f + (self check: 'NaN prints nan' is: nan printString = 'nan').
		^f
	]


	class infinity [
		| f inf ninf |
		f := 0.
		inf := Float infinity.
		ninf := Float infinity negated.
		f := f + (self check: '1/0 is inf' is: (1.0 / 0.0) = inf).
		f := f + (self check: '-1/0 is -inf' is: ((-1.0) / 0.0) = ninf).
		f := f + (self check: 'inf + inf = inf' is: (inf + inf) = inf).
		f := f + (self check: 'inf + finite = inf' is: (inf + 1.0e300) = inf).
		f := f + (self check: 'inf * 2 = inf' is: (inf * 2.0) = inf).
		f := f + (self check: 'inf * -1 = -inf' is: (inf * (-1.0)) = ninf).
		f := f + (self check: '1 / inf = 0' is: (1.0 / inf) = 0.0).
		f := f + (self check: 'inf > max double' is: inf > self maxDouble).
		f := f + (self check: '-inf < -max double' is: ninf < (0.0 - self maxDouble)).
		f := f + (self check: 'inf = inf' is: inf = inf).
		f := f + (self check: 'inf ~= -inf' is: inf ~= ninf).
		f := f + (self check: 'inf isInfinite' is: inf isInfinite).
		f := f + (self check: '-inf isInfinite' is: ninf isInfinite).
		f := f + (self check: 'inf is not finite' is: inf isFinite not).
		f := f + (self check: 'inf is not NaN' is: inf isNaN not).
		f := f + (self check: 'inf boxes' is: inf class == BoxedFloat64).
		f := f + (self check: 'inf prints inf' is: inf printString = 'inf').
		f := f + (self check: '-inf prints -inf' is: ninf printString = '-inf').
		f := f + (self check: 'inf > any integer' is: inf > 1000000000000).
		^f
	]


	class signedZero [
		| f nz |
		f := 0.
		nz := self negativeZero.
		f := f + (self check: '-0.0 equals 0.0' is: nz = 0.0).
		f := f + (self check: '-0.0 isZero' is: nz isZero).
		f := f + (self check: '-0.0 is immediate' is: nz class == SmallFloat64).
		f := f + (self check: '1 / -0.0 is -inf (sign survives)' is: (1.0 / nz) = Float infinity negated).
		f := f + (self check: '1 / +0.0 is +inf' is: (1.0 / 0.0) = Float infinity).
		f := f + (self check: '-0.0 + 0.0 is +0.0' is: (1.0 / (nz + 0.0)) = Float infinity).
		f := f + (self check: '-0.0 * -1 is +0.0' is: (1.0 / (nz * (-1.0))) = Float infinity).
		f := f + (self check: '0.0 is immediate' is: 0.0 class == SmallFloat64).
		^f
	]


	class rounding [
		| f big |
		f := 0.
		f := f + (self check: '0.5 rounded = 1 (half away from zero)' is: 0.5 rounded = 1).
		f := f + (self check: '-0.5 rounded = -1' is: (-0.5) rounded = -1).
		f := f + (self check: '2.5 rounded = 3' is: 2.5 rounded = 3).
		f := f + (self check: '-2.5 rounded = -3' is: (-2.5) rounded = -3).
		f := f + (self check: '2.4 rounded = 2' is: 2.4 rounded = 2).
		f := f + (self check: '2.7 truncated = 2 (toward zero)' is: 2.7 truncated = 2).
		f := f + (self check: '-2.7 truncated = -2' is: (-2.7) truncated = -2).
		f := f + (self check: '2.7 floor = 2' is: 2.7 floor = 2).
		f := f + (self check: '-2.7 floor = -3' is: (-2.7) floor = -3).
		f := f + (self check: '2.7 ceiling = 3' is: 2.7 ceiling = 3).
		f := f + (self check: '-2.7 ceiling = -2' is: (-2.7) ceiling = -2).
		f := f + (self check: 'asInteger = truncated' is: 2.7 asInteger = 2).
		f := f + (self check: 'small truncated is SmallInteger' is: 2.7 truncated class == SmallInteger).

		"beyond the SmallInteger range: the LargeInteger fallback is exact"
		big := 1.0 timesTwoPower: 61.
		f := f + (self check: '2^61 truncated is LargePositiveInteger' is: big truncated class == LargePositiveInteger).
		"NOTE the asymmetric two's complement range: -2^61 still fits the
		62-bit SmallInteger payload, +2^61 does not; only -2^62 promotes"
		f := f + (self check: 'negative 2^61 truncated stays SmallInteger'
			is: (0.0 - big) truncated class == SmallInteger).
		f := f + (self check: 'negative 2^62 truncated is LargeNegativeInteger'
			is: (0.0 - (1.0 timesTwoPower: 62)) truncated class == LargeNegativeInteger).
		f := f + (self check: '2^61 truncated is exact' is: big truncated = (2 raisedToInteger: 61)).
		f := f + (self check: '2^61 round-trips back' is: big truncated asFloat = big).
		f := f + (self check: 'negative huge truncated exact'
			is: (0.0 - big) truncated = (0 - (2 raisedToInteger: 61))).

		"the 2^53 precision cliff: above it doubles cannot hold odd integers"
		f := f + (self check: '2^53 + 1 rounds back to 2^53'
			is: ((1.0 timesTwoPower: 53) + 1.0) = (1.0 timesTwoPower: 53)).
		f := f + (self check: '2^52 + 1 is still exact'
			is: ((1.0 timesTwoPower: 52) + 1.0) truncated = ((2 raisedToInteger: 52) + 1)).
		^f
	]


	class precision [
		| f |
		f := 0.
		f := f + (self check: '0.1 + 0.2 is the classic sum' is: (0.1 + 0.2) = 0.30000000000000004).
		f := f + (self check: '0.1 + 0.2 ~= 0.3' is: (0.1 + 0.2) ~= 0.3).
		f := f + (self check: '1 + 2^-53 rounds to 1' is: (1.0 + (1.0 timesTwoPower: -53)) = 1.0).
		f := f + (self check: '1 + 2^-52 is above 1' is: (1.0 + (1.0 timesTwoPower: -52)) > 1.0).
		f := f + (self check: 'absorption at 1e16' is: (1.0e16 + 1.0 - 1.0e16) = 0.0).
		f := f + (self check: 'exact binary fractions associate' is: (0.25 + 0.5 + 0.125) = 0.875).
		^f
	]


	class coercion [
		| f |
		f := 0.
		f := f + (self check: 'int + float' is: (2 + 2.5) = 4.5).
		f := f + (self check: 'int + float class' is: (2 + 2.5) class == SmallFloat64).
		f := f + (self check: 'float + int' is: (2.5 + 2) = 4.5).
		f := f + (self check: 'float = int' is: 4.0 = 4).
		f := f + (self check: 'int = float' is: 4 = 4.0).
		f := f + (self check: 'int < float' is: 2 < 2.5).
		f := f + (self check: 'float < int' is: 2.5 < 3).
		f := f + (self check: 'fraction + float' is: ((1 / 2) + 0.5) = 1.0).
		f := f + (self check: 'float + fraction' is: (0.5 + (1 / 2)) = 1.0).
		f := f + (self check: 'fraction asFloat' is: (3 / 4) asFloat = 0.75).
		f := f + (self check: 'int asFloat is immediate' is: 7 asFloat class == SmallFloat64).
		f := f + (self check: 'LargeInteger asFloat' is: (2 raisedToInteger: 100) asFloat = (1.0 timesTwoPower: 100)).
		f := f + (self check: 'LargeInteger asFloat boxes when huge'
			is: (2 raisedToInteger: 300) asFloat class == BoxedFloat64).
		f := f + (self check: 'huge float = its truncation' is: 1.0e300 = 1.0e300 truncated).
		^f
	]


	class printing [
		| f |
		f := 0.
		f := f + (self check: 'shortest round-trip 3.14' is: 3.14 printString = '3.14').
		f := f + (self check: 'integral float keeps .0' is: 1500.0 printString = '1500.0').
		f := f + (self check: 'huge magnitude prints exponent' is: 1.0e300 printString = '1e+300').
		f := f + (self check: 'classic sum prints all digits'
			is: (0.1 + 0.2) printString = '0.30000000000000004').
		f := f + (self check: 'min subnormal prints shortest' is: 5e-324 printString = '5e-324').
		f := f + (self check: 'zero prints 0.0' is: 0.0 printString = '0.0').
		^f
	]


	class exceptions [
		| f caught |
		f := 0.
		caught := false.
		[Float nan truncated] on: Error do: [:e | caught := true].
		f := f + (self check: 'NaN truncated raises a catchable Error' is: caught).
		caught := false.
		[Float infinity truncated] on: Error do: [:e | caught := true].
		f := f + (self check: 'inf truncated raises a catchable Error' is: caught).
		caught := false.
		[0.0 exponent] on: Error do: [:e | caught := true].
		f := f + (self check: 'zero has no exponent' is: caught).
		caught := false.
		[Float nan rounded] on: Error do: [:e | caught := true].
		f := f + (self check: 'NaN rounded raises too' is: caught).
		^f
	]


	class runAll [
		| total |
		total := self overflow.
		total := total + self underflow.
		total := total + self nan.
		total := total + self infinity.
		total := total + self signedZero.
		total := total + self rounding.
		total := total + self precision.
		total := total + self coercion.
		total := total + self printing.
		total := total + self exceptions.
		total = 0
			ifTrue: [Transcript nextPutAll: 'FloatEdgeTest: all sections passed'; lf]
			ifFalse: [Transcript nextPutAll: 'FloatEdgeTest: ', total printString, ' FAILURES'; lf].
		^total
	]

]


[
	^FloatEdgeTest runAll
]
