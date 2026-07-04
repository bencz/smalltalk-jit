"Exact rational number. Instances are always kept in lowest terms with a
positive denominator strictly greater than 1 -- a fraction whose denominator
reduces to 1 is demoted to an Integer by #numerator:denominator:. Produced by
Integer>>/ and by mixed Integer/Fraction arithmetic."

Fraction := Number [

	| numerator denominator |


	"--- instance creation ---"

	class numerator: n denominator: d [
		"Answer n/d reduced to lowest terms, as an Integer when the denominator
		reduces to 1."
		| num den g |

		d = 0 ifTrue: [^self error: 'division by zero'].
		num := n.
		den := d.
		den < 0 ifTrue: [num := num negated. den := den negated].
		g := num gcd: den.
		num := num // g.
		den := den // g.
		den = 1 ifTrue: [^num].
		^self basicNumerator: num denominator: den
	]


	class basicNumerator: n denominator: d [
		"Answer a Fraction without reducing or demoting (n/d assumed already
		normalized). Used for coercion of integers."
		^self new setNumerator: n denominator: d
	]


	setNumerator: n denominator: d [
		numerator := n.
		denominator := d.
		^self
	]


	"--- accessing ---"

	numerator [
		^numerator
	]


	denominator [
		^denominator
	]


	"--- arithmetic ---"

	+ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			^Fraction numerator: numerator + (aNumber * denominator) denominator: denominator].
		(aNumber isKindOf: Fraction) ifTrue: [
			^Fraction
				numerator: (numerator * aNumber denominator) + (aNumber numerator * denominator)
				denominator: denominator * aNumber denominator].
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			^Fraction numerator: numerator - (aNumber * denominator) denominator: denominator].
		(aNumber isKindOf: Fraction) ifTrue: [
			^Fraction
				numerator: (numerator * aNumber denominator) - (aNumber numerator * denominator)
				denominator: denominator * aNumber denominator].
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			^Fraction numerator: numerator * aNumber denominator: denominator].
		(aNumber isKindOf: Fraction) ifTrue: [
			^Fraction
				numerator: numerator * aNumber numerator
				denominator: denominator * aNumber denominator].
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^self error: 'division by zero'].
			^Fraction numerator: numerator denominator: denominator * aNumber].
		(aNumber isKindOf: Fraction) ifTrue: [
			^Fraction
				numerator: numerator * aNumber denominator
				denominator: denominator * aNumber numerator].
		^self retry: #/ coercing: aNumber
	]


	// aNumber [
		^(self / aNumber) floor
	]


	\\ aNumber [
		^self - ((self // aNumber) * aNumber)
	]


	negated [
		^Fraction basicNumerator: numerator negated denominator: denominator
	]


	abs [
		^Fraction basicNumerator: numerator abs denominator: denominator
	]


	reciprocal [
		^Fraction numerator: denominator denominator: numerator
	]


	squared [
		^Fraction numerator: numerator * numerator denominator: denominator * denominator
	]


	"--- comparing ---"

	< aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^numerator < (aNumber * denominator)].
		(aNumber isKindOf: Fraction) ifTrue: [
			^(numerator * aNumber denominator) < (aNumber numerator * denominator)].
		^self retryRelationalOp: #< coercing: aNumber
	]


	= aNumber [
		(aNumber isKindOf: Fraction) ifTrue: [
			^numerator = aNumber numerator and: [denominator = aNumber denominator]].
		(aNumber isKindOf: Integer) ifTrue: [^false].
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	hash [
		^numerator hash bitXor: denominator hash
	]


	"--- testing ---"

	sign [
		^numerator sign
	]


	isZero [
		^numerator = 0
	]


	"--- truncation and rounding ---"

	truncated [
		^numerator quo: denominator
	]


	floor [
		^numerator // denominator
	]


	ceiling [
		^self negated floor negated
	]


	rounded [
		^(self + (Fraction basicNumerator: 1 denominator: 2)) floor
	]


	"--- coercing ---"

	generality [
		^400
	]


	coerce: aNumber [
		"Wrap an Integer as a fraction with denominator 1 for mixed arithmetic."
		^Fraction basicNumerator: aNumber denominator: 1
	]


	asFraction [
		^self
	]


	asFloat [
		^numerator asFloat / denominator asFloat
	]


	"--- printing ---"

	printOn: aStream [
		numerator printOn: aStream.
		aStream nextPut: $/.
		denominator printOn: aStream.
	]

]
