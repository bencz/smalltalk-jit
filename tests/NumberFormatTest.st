"NumberFormatTest: wave 3 numeric printing and utilities. Generic
 Integer>>printStringBase:/printString: (working for LargeInteger too),
 printPaddedWith:to:, Float>>printShowingDecimalPlaces: (half-up),
 Integer>>sqrtFloor/isPrime, the bit-access helpers (bitAt:/setBit:/clearBit:/
 lowBit/bitCount), and Number>>clampBetween:and:."

[
	| t |
	t := TestRun named: 'NumberFormat'.

	"---- printStringBase: for SmallInteger ----"
	t check: 'base 2 of 10' equals: '1010' is: [ 10 printStringBase: 2 ].
	t check: 'base 8 of 8' equals: '10' is: [ 8 printStringBase: 8 ].
	t check: 'base 16 of 255' equals: 'FF' is: [ 255 printStringBase: 16 ].
	t check: 'base 16 of negative' equals: '-FF' is: [ 255 negated printStringBase: 16 ].
	t check: 'base 2 of 0' equals: '0' is: [ 0 printStringBase: 2 ].
	t check: 'printString: alias' equals: 'FF' is: [ 255 printString: 16 ].

	"---- printStringBase: for LargeInteger (2 raisedTo: 100) ----"
	t check: 'large base 16 is 1 then 25 zeros' equals: ('1' , ('0' repeat: 25)) is: [
		(2 raisedTo: 100) printStringBase: 16 ].
	t check: 'large base 2 is 1 then 100 zeros' equals: ('1' , ('0' repeat: 100)) is: [
		(2 raisedTo: 100) printStringBase: 2 ].
	t check: 'large base 8 is 2 then 33 zeros' equals: ('2' , ('0' repeat: 33)) is: [
		(2 raisedTo: 100) printStringBase: 8 ].
	t check: 'large negative base 16' equals: ('-1' , ('0' repeat: 25)) is: [
		(2 raisedTo: 100) negated printStringBase: 16 ].

	"---- printPaddedWith:to: ----"
	t check: 'pad to width' equals: '00042' is: [ 42 printPaddedWith: $0 to: 5 ].
	t check: 'pad already wide enough' equals: '42' is: [ 42 printPaddedWith: $0 to: 2 ].
	t check: 'pad with spaces' equals: '  42' is: [ 42 printPaddedWith: Character space to: 4 ].

	"---- printShowingDecimalPlaces: (half-up) ----"
	t check: '2.5 to 1 place' equals: '2.5' is: [ 2.5 printShowingDecimalPlaces: 1 ].
	t check: '0.125 to 2 places rounds half up' equals: '0.13' is: [ 0.125 printShowingDecimalPlaces: 2 ].
	t check: 'whole float to 2 places' equals: '3.00' is: [ 3.0 printShowingDecimalPlaces: 2 ].
	t check: 'negative to 1 place' equals: '-2.5' is: [ 2.5 negated printShowingDecimalPlaces: 1 ].
	t check: 'zero to 2 places' equals: '0.00' is: [ 0.0 printShowingDecimalPlaces: 2 ].
	t check: 'truncating extra digits' equals: '1.23' is: [ 1.2345 printShowingDecimalPlaces: 2 ].

	"---- sqrtFloor ----"
	t check: 'sqrtFloor of perfect square 16' equals: 4 is: [ 16 sqrtFloor ].
	t check: 'sqrtFloor of perfect square 144' equals: 12 is: [ 144 sqrtFloor ].
	t check: 'sqrtFloor of non-perfect 15' equals: 3 is: [ 15 sqrtFloor ].
	t check: 'sqrtFloor of non-perfect 10' equals: 3 is: [ 10 sqrtFloor ].
	t check: 'sqrtFloor of 0' equals: 0 is: [ 0 sqrtFloor ].
	t check: 'sqrtFloor of 1' equals: 1 is: [ 1 sqrtFloor ].
	t check: 'sqrtFloor of large perfect square' equals: (10 raisedTo: 25) is: [
		(10 raisedTo: 50) sqrtFloor ].
	t check: 'sqrtFloor just below large perfect square' equals: ((10 raisedTo: 25) - 1) is: [
		((10 raisedTo: 50) - 1) sqrtFloor ].
	t check: 'sqrtFloor just above large perfect square' equals: (10 raisedTo: 25) is: [
		((10 raisedTo: 50) + 1) sqrtFloor ].

	"---- isPrime ----"
	t check: 'isPrime 2' that: [ 2 isPrime ].
	t check: 'isPrime 3' that: [ 3 isPrime ].
	t check: 'isPrime 97' that: [ 97 isPrime ].
	t check: 'isPrime 4 false' that: [ 4 isPrime not ].
	t check: 'isPrime 25 false' that: [ 25 isPrime not ].
	t check: 'isPrime 1 false' that: [ 1 isPrime not ].
	t check: 'isPrime 0 false' that: [ 0 isPrime not ].

	"---- bit access ----"
	t check: 'bitAt: set bit' equals: 1 is: [ 8 bitAt: 4 ].
	t check: 'bitAt: clear bit' equals: 0 is: [ 8 bitAt: 1 ].
	t check: 'setBit: on zero' equals: 4 is: [ 0 setBit: 3 ].
	t check: 'setBit: combines' equals: 5 is: [ 1 setBit: 3 ].
	t check: 'clearBit:' equals: 14 is: [ 15 clearBit: 1 ].
	t check: 'clearBit: of unset bit is a no-op' equals: 8 is: [ 8 clearBit: 1 ].
	t check: 'lowBit of 12' equals: 3 is: [ 12 lowBit ].
	t check: 'lowBit of 1' equals: 1 is: [ 1 lowBit ].
	t check: 'lowBit of 0' equals: 0 is: [ 0 lowBit ].
	t check: 'bitCount of 7' equals: 3 is: [ 7 bitCount ].
	t check: 'bitCount of 255' equals: 8 is: [ 255 bitCount ].
	t check: 'bitCount of 0' equals: 0 is: [ 0 bitCount ].

	"---- clampBetween:and: ----"
	t check: 'clamp above range' equals: 3 is: [ 5 clampBetween: 1 and: 3 ].
	t check: 'clamp below range' equals: 1 is: [ 0 clampBetween: 1 and: 3 ].
	t check: 'clamp inside range' equals: 2 is: [ 2 clampBetween: 1 and: 3 ].
	t check: 'clamp a float' equals: 2 is: [ 2.5 clampBetween: 0 and: 2 ].

	^t report
]
