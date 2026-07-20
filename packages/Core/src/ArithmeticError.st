"Root of the arithmetic error family (division by zero, overflow, domain
 errors). A plain Error: not resumable, since the faulting operation cannot be
 meaningfully continued."

ArithmeticError := Error [

	defaultMessageText [
		^'arithmetic error'
	]

]
