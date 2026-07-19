"Signaled when a division, modulo or remainder is attempted with a zero
 divisor. Being an ArithmeticError (and therefore an Error) it is not
 resumable. The optional dividend records the numerator at the signal point."

ZeroDivide := ArithmeticError [

	| dividend |


	"instance creation"

	class dividend: aNumber [
		^self new setDividend: aNumber
	]


	"accessing"

	setDividend: aNumber [
		dividend := aNumber.
		^self
	]


	dividend [
		^dividend
	]


	defaultMessageText [
		^'division by zero'
	]

]
