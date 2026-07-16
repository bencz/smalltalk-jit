"ScaledDecimal: exact decimal numbers with a print scale. The value is an
exact rational, so decimal arithmetic has NO binary float rounding; `scale`
only controls how many decimal places printing shows (rounded half away from
zero). Fails-counter pattern: a bare Assert failure exits 0, only the final
^total is caught by run_tests.sh."

ScaledDecimalTest := Object [

	class check: name is: ok [
		ok ifTrue: [^0].
		Transcript nextPutAll: 'FAIL: ', name; lf.
		^1
	]


	class literals [
		| f |
		f := 0.
		f := f + (self check: 'literal class' is: 3.14s2 class == ScaledDecimal).
		f := f + (self check: 'literal is a Number' is: (3.14s2 isKindOf: Number)).
		f := f + (self check: 'explicit scale' is: 3.14s2 scale = 2).
		f := f + (self check: 'inferred scale' is: 2.5s scale = 1).
		f := f + (self check: 'integer literal with scale' is: 3s2 scale = 2).
		f := f + (self check: 'bare s means scale zero' is: 42s scale = 0).
		f := f + (self check: 'wider explicit scale' is: 3.14s4 scale = 4).
		f := f + (self check: 'negative literal' is: (-3.14s2) < 0).
		f := f + (self check: 'literal value is exact' is: 3.14s2 = (314 / 100)).
		f := f + (self check: 'huge numerator literal'
			is: 12345678901234567890.5s1 = (123456789012345678905 / 10)).
		^f
	]


	class printing [
		| f |
		f := 0.
		f := f + (self check: 'plain print' is: 3.14s2 printString = '3.14s2').
		f := f + (self check: 'padding to the scale' is: 3.5s3 printString = '3.500s3').
		f := f + (self check: 'integer value keeps decimals' is: 3s2 printString = '3.00s2').
		f := f + (self check: 'scale zero has no point' is: 42s printString = '42s0').
		f := f + (self check: 'negative print' is: (-3.14s2) printString = '-3.14s2').
		f := f + (self check: 'leading zero' is: 0.5s1 printString = '0.5s1').
		f := f + (self check: 'zero print' is: 0s2 printString = '0.00s2').
		f := f + (self check: 'print rounds half away from zero'
			is: (1s2 / 8) printString = '0.13s2').
		f := f + (self check: 'print rounding does not change the value'
			is: ((1s2 / 8) * 8) = 1).
		f := f + (self check: 'repeating decimal prints rounded'
			is: (2s1 / 3) printString = '0.7s1').
		^f
	]


	class exactArithmetic [
		| f |
		f := 0.
		f := f + (self check: 'THE decimal sum is exact (unlike Float)'
			is: (0.1s1 + 0.2s1) = (3 / 10)).
		f := f + (self check: 'exact sum prints clean' is: (0.1s1 + 0.2s1) printString = '0.3s1').
		f := f + (self check: 'sum class' is: (0.1s1 + 0.2s1) class == ScaledDecimal).
		f := f + (self check: 'scale propagates as the max'
			is: (1.5s1 + 2.25s2) printString = '3.75s2').
		f := f + (self check: 'subtraction' is: (1.5s1 - 0.25s2) printString = '1.25s2').
		f := f + (self check: 'multiplication' is: (1.5s1 * 2.5s1) printString = '3.8s1').
		f := f + (self check: 'multiplication value stays exact' is: (1.5s1 * 2.5s1) = (15 / 4)).
		f := f + (self check: 'division stays exact' is: ((2s1 / 3) * 3) = 2).
		f := f + (self check: 'floored division' is: (7.5s1 // 2) = 3).
		f := f + (self check: 'modulo' is: (7.5s1 \\ 2) = (3 / 2)).
		f := f + (self check: 'negated' is: 3.14s2 negated printString = '-3.14s2').
		f := f + (self check: 'abs' is: (-3.14s2) abs printString = '3.14s2').
		f := f + (self check: 'sign' is: (-3.14s2) sign = -1).
		f := f + (self check: 'isZero' is: (0.1s1 - 0.1s1) isZero).
		^f
	]


	class mixedTower [
		| f |
		f := 0.
		f := f + (self check: 'sd + integer' is: (1.5s1 + 1) printString = '2.5s1').
		f := f + (self check: 'integer + sd promotes' is: (1 + 1.5s1) class == ScaledDecimal).
		f := f + (self check: 'integer + sd value' is: (1 + 1.5s1) printString = '2.5s1').
		f := f + (self check: 'sd + fraction' is: ((1 / 2) + 0.25s2) printString = '0.75s2').
		f := f + (self check: 'fraction + sd class' is: ((1 / 2) + 0.25s2) class == ScaledDecimal).
		f := f + (self check: 'sd equals fraction' is: 0.5s1 = (1 / 2)).
		f := f + (self check: 'fraction equals sd' is: (1 / 2) = 0.5s1).
		f := f + (self check: 'sd equals integer' is: 2s0 = 2).
		f := f + (self check: 'integer equals sd' is: 2 = 2.00s2).
		f := f + (self check: 'hash agrees with the fraction' is: 0.5s1 hash = (1 / 2) hash).
		f := f + (self check: 'hash agrees with the integer' is: 2.00s2 hash = 2 hash).
		f := f + (self check: 'ordering vs integer' is: 1.5s1 < 2).
		f := f + (self check: 'ordering vs fraction' is: (1 / 4) < 0.5s1).
		f := f + (self check: 'sd + float coerces to float' is: (0.5s1 + 0.25) = 0.75).
		f := f + (self check: 'sd + float class' is: (0.5s1 + 0.25) class == SmallFloat64).
		f := f + (self check: 'sd equals float' is: 0.5s1 = 0.5).
		f := f + (self check: 'float equals sd' is: 0.5 = 0.5s1).
		f := f + (self check: 'sd asFloat' is: 0.5s1 asFloat = 0.5).
		f := f + (self check: 'large integer promotion inside sd math'
			is: (1000000000000000000s0 * 1000000000000000000s0)
				= (10 raisedToInteger: 36)).
		^f
	]


	class truncationRounding [
		| f |
		f := 0.
		f := f + (self check: 'truncated toward zero' is: 2.7s1 truncated = 2).
		f := f + (self check: 'negative truncated' is: (-2.7s1) truncated = -2).
		f := f + (self check: 'floor' is: 2.7s1 floor = 2).
		f := f + (self check: 'negative floor' is: (-2.7s1) floor = -3).
		f := f + (self check: 'ceiling' is: 2.3s1 ceiling = 3).
		f := f + (self check: 'negative ceiling' is: (-2.7s1) ceiling = -2).
		f := f + (self check: 'rounded half up' is: 2.5s1 rounded = 3).
		f := f + (self check: 'rounded down' is: 2.4s1 rounded = 2).
		f := f + (self check: 'asInteger is truncated' is: 2.7s1 asInteger = 2).
		f := f + (self check: 'truncated class' is: 2.7s1 truncated class == SmallInteger).
		^f
	]


	class runAll [
		| total |
		total := self literals.
		total := total + self printing.
		total := total + self exactArithmetic.
		total := total + self mixedTower.
		total := total + self truncationRounding.
		total = 0
			ifTrue: [Transcript nextPutAll: 'ScaledDecimalTest: all sections passed'; lf]
			ifFalse: [Transcript nextPutAll: 'ScaledDecimalTest: ', total printString, ' FAILURES'; lf].
		^total
	]

]


[
	^ScaledDecimalTest runAll
]
