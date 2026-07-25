"Number protocol completeness and the SILENT WRONG ANSWERS it used to give.

 Six methods in Number.st had EMPTY bodies (even, odd, ceiling, floor,
 truncated, rounded), so any subclass that did not override them answered the
 RECEIVER instead: `3.0 even` answered 3.0, `(3/4) even` answered 3/4. An empty
 body is the worst failure mode available, because it is indistinguishable from
 a correct answer at the call site. They are now either a correct generic
 derivation (even, odd, ceiling, floor, rounded, all expressed in terms of
 truncated) or a loud subclassResponsibility (truncated, the only genuinely
 representation-specific one).

 Also covered: the class-membership predicates (isNumber/isInteger/isFloat/
 isFraction/isCharacter/isString/isSymbol, none of which existed, so
 `3.0 isInteger` was a doesNotUnderstand), the total conversion protocol
 between Integer, Fraction and Float, ceilingLog: (which Integer>>floorLog: has
 always called for a radix below 1 without it existing), and the SmallInteger
 range boundary, which is ASYMMETRIC."

"A Number subclass that implements the bare minimum. Every rounding method must
 still work through the generic derivations, and truncated must be the only
 thing it had to supply."
MinimalNumber := Number [
	| value |
	class value: aNumber [ ^self new setValue: aNumber ]
	setValue: aNumber [ value := aNumber. ^self ]
	value [ ^value ]
	truncated [ ^value truncated ]
	< aNumber [ ^value < aNumber ]
	> aNumber [ ^value > aNumber ]
	= aNumber [ ^value = aNumber ]
	hash [ ^value hash ]
	- aNumber [ ^value - aNumber ]
	+ aNumber [ ^value + aNumber ]
	negative [ ^value < 0 ]
]

"A Number subclass that supplies NOTHING. truncated must raise, not answer self."
UselessNumber := Number [ ]

[ | t |
	t := TestRun named: 'NumberProtocol'.

	"--- the former empty bodies, on the classes that never overrode them ---"
	t check: '3.0 even is false, not 3.0' equals: false is: [ 3.0 even ].
	t check: '4.0 even is true' equals: true is: [ 4.0 even ].
	t check: '3.0 odd is true' equals: true is: [ 3.0 odd ].
	t check: 'a Fraction is never even' equals: false is: [ (3/4) even ].
	t check: 'a Fraction is never odd either' equals: false is: [ (3/4) odd ].
	t check: 'negative even still works' equals: true is: [ -4.0 even ].

	"--- truncated is the one genuinely abstract member: it must RAISE ---"
	t expect: 'a Number subclass with no truncated raises' raises: Error in: [
		UselessNumber new truncated ].

	"--- and everything else derives from it ---"
	t check: 'derived floor, positive' equals: 2 is: [ (MinimalNumber value: 2.5) floor ].
	t check: 'derived floor, negative' equals: -3 is: [ (MinimalNumber value: -2.5) floor ].
	t check: 'derived ceiling, positive' equals: 3 is: [ (MinimalNumber value: 2.5) ceiling ].
	t check: 'derived ceiling, negative' equals: -2 is: [ (MinimalNumber value: -2.5) ceiling ].
	t check: 'derived floor of an exact integer' equals: 4 is: [ (MinimalNumber value: 4.0) floor ].
	t check: 'derived ceiling of an exact integer' equals: 4 is: [ (MinimalNumber value: 4.0) ceiling ].
	t check: 'derived rounded, ties away from zero' equals: 3 is: [ (MinimalNumber value: 2.5) rounded ].
	t check: 'derived rounded, negative tie' equals: -3 is: [ (MinimalNumber value: -2.5) rounded ].
	t check: 'derived rounded, below the tie' equals: 2 is: [ (MinimalNumber value: 2.4) rounded ].

	"the generic derivations must agree with the concrete overrides"
	t check: 'generic floor agrees with Float floor' equals: -2.5 floor
		is: [ (MinimalNumber value: -2.5) floor ].
	t check: 'generic ceiling agrees with Float ceiling' equals: -2.5 ceiling
		is: [ (MinimalNumber value: -2.5) ceiling ].
	t check: 'generic rounded agrees with Float rounded' equals: 2.5 rounded
		is: [ (MinimalNumber value: 2.5) rounded ].

	"--- class-membership predicates ---"
	t check: '3 isNumber' equals: true is: [ 3 isNumber ].
	t check: '3.0 isNumber' equals: true is: [ 3.0 isNumber ].
	t check: '(3/4) isNumber' equals: true is: [ (3/4) isNumber ].
	t check: 'a String is not a Number' equals: false is: [ 'x' isNumber ].
	t check: '3 isInteger' equals: true is: [ 3 isInteger ].
	t check: '3.0 isInteger is false, and does not raise' equals: false is: [ 3.0 isInteger ].
	t check: '3.0 isFloat' equals: true is: [ 3.0 isFloat ].
	t check: '3 isFloat' equals: false is: [ 3 isFloat ].
	t check: '(3/4) isFraction' equals: true is: [ (3/4) isFraction ].
	t check: '3 isFraction' equals: false is: [ 3 isFraction ].
	t check: '$a isCharacter' equals: true is: [ $a isCharacter ].
	t check: '3 isCharacter' equals: false is: [ 3 isCharacter ].
	t check: 'a String isString' equals: true is: [ 'abc' isString ].
	t check: 'a Symbol isString too' equals: true is: [ #abc isString ].
	t check: 'a Symbol isSymbol' equals: true is: [ #abc isSymbol ].
	t check: 'a String is not a Symbol' equals: false is: [ 'abc' isSymbol ].
	t check: 'nil answers false, not an error' equals: false is: [ nil isNumber ].

	"--- ceilingLog:, and the floorLog: branch that always called it ---"
	t check: 'ceilingLog: rounds up' equals: 4 is: [ 10 ceilingLog: 2 ].
	t check: 'ceilingLog: exact power' equals: 3 is: [ 8 ceilingLog: 2 ].
	t check: 'floorLog: with a radix below 1 no longer raises' equals: -4
		is: [ 10 floorLog: (1/2) ].

	"--- total conversion protocol ---"
	t check: '(3/2) asInteger truncates toward zero' equals: 1 is: [ (3/2) asInteger ].
	t check: '(-3/2) asInteger truncates toward zero' equals: -1 is: [ (-3/2) asInteger ].
	t check: 'Integer asFraction stays an Integer' equals: 5 is: [ 5 asFraction ].
	t check: 'Integer numerator' equals: 5 is: [ 5 numerator ].
	t check: 'Integer denominator' equals: 1 is: [ 5 denominator ].
	t check: 'Fraction numerator still works' equals: 3 is: [ (3/4) numerator ].
	t check: 'Fraction denominator still works' equals: 4 is: [ (3/4) denominator ].

	"asExactFraction is EXACT: 0.1 is not one tenth, and must not pretend to be"
	t check: '0.75 is exactly 3/4' equals: (3/4) is: [ 0.75 asExactFraction ].
	t check: '0.5 is exactly 1/2' equals: (1/2) is: [ 0.5 asExactFraction ].
	t check: 'an integral Float converts to an Integer' equals: 4 is: [ 4.0 asExactFraction ].
	t check: '0.0 converts to 0' equals: 0 is: [ 0.0 asExactFraction ].
	t check: '-0.75 is exactly -3/4' equals: (-3/4) is: [ -0.75 asExactFraction ].
	t check: '0.1 is NOT one tenth' equals: false is: [ 0.1 asExactFraction = (1/10) ].
	t check: 'but it round-trips back to 0.1' equals: 0.1 is: [ 0.1 asExactFraction asFloat ].
	t check: 'asFraction is asExactFraction' equals: (3/4) is: [ 0.75 asFraction ].

	"--- SmallInteger range, which is ASYMMETRIC ---"
	t check: 'maxVal is immediate' equals: SmallInteger
		is: [ SmallInteger maxVal class ].
	t check: 'one past maxVal promotes' equals: LargePositiveInteger
		is: [ (SmallInteger maxVal + 1) class ].
	t check: 'minVal is immediate' equals: SmallInteger
		is: [ SmallInteger minVal class ].
	t check: 'one past minVal promotes' equals: LargeNegativeInteger
		is: [ (SmallInteger minVal - 1) class ].
	t check: 'the range is asymmetric: maxVal negated is NOT minVal' equals: false
		is: [ SmallInteger maxVal negated = SmallInteger minVal ].
	t check: 'promotion round-trips back to immediate' equals: SmallInteger
		is: [ (SmallInteger maxVal + 1 - 1) class ].
	t check: 'and back to the same value' equals: SmallInteger maxVal
		is: [ SmallInteger maxVal + 1 - 1 ].

	^t report
]
