"============================================================
  09 - Recursion
  ------------------------------------------------------------
  Classic recursive algorithms, implemented as methods on a
  helper class. Recursion in Smalltalk is just a method sending
  a message to self (or another object of the same kind).
  ============================================================"

Recurse := Object [

	"n! = n * (n-1)!"
	factorial: n [
		n <= 1 ifTrue: [^1].
		^n * (self factorial: n - 1)
	]

	"Naive Fibonacci."
	fib: n [
		n < 2 ifTrue: [^n].
		^(self fib: n - 1) + (self fib: n - 2)
	]

	"Euclid's greatest common divisor."
	gcd: a and: b [
		b = 0 ifTrue: [^a].
		^self gcd: b and: a \\ b
	]

	"Sum of decimal digits."
	digitSum: n [
		n < 10 ifTrue: [^n].
		^(n \\ 10) + (self digitSum: n // 10)
	]

	"Towers of Hanoi: print the moves, return how many were made."
	moveDisks: n from: source to: target via: spare [
		| count |
		n = 0 ifTrue: [^0].
		count := self moveDisks: n - 1 from: source to: spare via: target.
		Transcript
			nextPutAll: '  move disk '; nextPutAll: n printString;
			nextPutAll: ' from '; nextPutAll: source;
			nextPutAll: ' to '; nextPutAll: target; lf.
		count := count + 1 + (self moveDisks: n - 1 from: spare to: target via: source).
		^count
	]
]

[
	| r |
	r := Recurse new.
	Transcript nextPutAll: '10 factorial = '. (r factorial: 10) printNl.
	Transcript nextPutAll: 'fib(15)      = '. (r fib: 15) printNl.
	Transcript nextPutAll: 'gcd(48, 36)  = '. (r gcd: 48 and: 36) printNl.
	Transcript nextPutAll: 'digitSum 9375 = '. (r digitSum: 9375) printNl.
]

[
	| r moves |
	r := Recurse new.
	Transcript nextPutAll: 'Towers of Hanoi (3 disks):'; lf.
	moves := r moveDisks: 3 from: 'A' to: 'C' via: 'B'.
	Transcript nextPutAll: 'solved in '; nextPutAll: moves printString;
		nextPutAll: ' moves'; lf.
]
