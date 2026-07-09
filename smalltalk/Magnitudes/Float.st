"Double precision floating point number. The IEEE-754 double is stored in the
object's (unscanned) payload word; all access goes through primitives. Instances
are created from float literals (3.14, 1.5e-3) and by #asFloat coercion."

Float := Number [

	<shape: FloatShape>


	"instance creation"

	class pi [
		^3.141592653589793
	]


	class e [
		^2.718281828459045
	]


	class infinity [
		^1.0 / 0.0
	]


	class nan [
		^0.0 / 0.0
	]


	"arithmetic"

	+ aNumber [
		<primitive: FloatAddPrimitive>
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		<primitive: FloatSubPrimitive>
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		<primitive: FloatMulPrimitive>
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		<primitive: FloatDivPrimitive>
		^self retry: #/ coercing: aNumber
	]


	// aNumber [
		^(self / aNumber) floor
	]


	\\ aNumber [
		^self - ((self // aNumber) * aNumber)
	]


	negated [
		^0.0 - self
	]


	"comparing"

	< aNumber [
		<primitive: FloatLessThanPrimitive>
		^self retryRelationalOp: #< coercing: aNumber
	]


	= aNumber [
		<primitive: FloatEqualsPrimitive>
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	hash [
		^self truncated hash
	]


	"testing"

	isZero [
		^self = 0.0
	]


	"mathematical functions"

	sqrt [
		<primitive: FloatSqrtPrimitive>
	]


	sin [
		<primitive: FloatSinPrimitive>
	]


	cos [
		<primitive: FloatCosPrimitive>
	]


	tan [
		<primitive: FloatTanPrimitive>
	]


	arcSin [
		<primitive: FloatArcSinPrimitive>
	]


	arcCos [
		<primitive: FloatArcCosPrimitive>
	]


	arcTan [
		<primitive: FloatArcTanPrimitive>
	]


	arcTan: aNumber [
		"Two-argument arctangent: self arcTan: aNumber == atan2(self, aNumber)."
		<primitive: FloatArcTan2Primitive>
		^self arcTan: aNumber asFloat
	]


	exp [
		<primitive: FloatExpPrimitive>
	]


	ln [
		<primitive: FloatLnPrimitive>
	]


	log: aNumber [
		^self ln / aNumber ln
	]


	"truncation and rounding"

	truncated [
		<primitive: FloatTruncatedPrimitive>
		^self error: 'Float is out of the integer range'
	]


	floor [
		<primitive: FloatFloorPrimitive>
		^self error: 'Float is out of the integer range'
	]


	ceiling [
		<primitive: FloatCeilingPrimitive>
		^self error: 'Float is out of the integer range'
	]


	rounded [
		<primitive: FloatRoundedPrimitive>
		^self error: 'Float is out of the integer range'
	]


	"coercing"

	generality [
		^500
	]


	coerce: aNumber [
		^aNumber asFloat
	]


	asFloat [
		^self
	]


	asInteger [
		^self truncated
	]


	"printing"

	printString [
		<primitive: FloatAsStringPrimitive>
	]


	printOn: aStream [
		aStream nextPutAll: self printString.
	]

]
