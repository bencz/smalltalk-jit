"============================================================
  20 - RPN Calculator
  ------------------------------------------------------------
  Evaluate arithmetic written in Reverse Polish Notation
  ('3 4 +'  ==>  7) using an operand stack. Operands are pushed;
  an operator pops two values and pushes the result.
  ============================================================"

RPNCalculator := Object [

	isOperator: aToken [
		^#('+' '-' '*' '/') includes: aToken
	]

	apply: op to: a and: b [
		op = '+' ifTrue: [^a + b].
		op = '-' ifTrue: [^a - b].
		op = '*' ifTrue: [^a * b].
		op = '/' ifTrue: [^a / b].
		^self error: 'unknown operator ' , op
	]

	evaluate: expression [
		| stack |
		stack := OrderedCollection new.
		(expression splitBy: Character space) do: [:token |
			(self isOperator: token)
				ifTrue: [
					| b a |
					b := stack removeLast.
					a := stack removeLast.
					stack addLast: (self apply: token to: a and: b)]
				ifFalse: [
					stack addLast: token asNumber]].
		^stack last
	]
]

[
	| calc |
	calc := RPNCalculator new.
	#('3 4 +' '5 1 2 + 4 * + 3 -' '2 3 4 * +' '100 5 / 3 -') do: [:expr |
		Transcript
			nextPutAll: '  '; nextPutAll: expr;
			nextPutAll: '   =   '; nextPutAll: (calc evaluate: expr) printString; lf].
]
