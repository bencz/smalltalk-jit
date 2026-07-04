"============================================================
  04 - Blocks and Closures
  ------------------------------------------------------------
  Blocks [ ... ] are first-class objects. They can be stored in
  variables, passed around, and they capture ('close over') the
  variables of their enclosing scope.

  NOTE: temporary variables (| ... |) must be declared at the
  very top of a block, before any statement.
  ============================================================"

"---- A block is an object; you run it with value / value: ----"
[
	| square hypotenuse |
	square := [:x | x * x].
	Transcript nextPutAll: 'square value: 9 = '.
	(square value: 9) printNl.

	"Multiple arguments with value:value:"
	hypotenuse := [:a :b | (a * a + (b * b)) sqrt].
	Transcript nextPutAll: 'hypotenuse 3,4 = '.
	(hypotenuse value: 3 value: 4) printNl.
]

"---- Closures capture mutable state ----"
[
	| makeCounter c1 c2 |
	"makeCounter returns a NEW block each time; each keeps its own count."
	makeCounter := [
		| count |
		count := 0.
		[count := count + 1. count]].

	c1 := makeCounter value.
	c2 := makeCounter value.

	Transcript nextPutAll: 'c1: '.
	c1 value printNl.        "1"
	c1 value printNl.        "2"
	Transcript nextPutAll: 'c2 (independent): '.
	c2 value printNl.        "1 - separate closure"
]

"---- Blocks as higher-order arguments ----"
[
	| applyTwice |
	applyTwice := [:f :x | f value: (f value: x)].
	Transcript nextPutAll: 'applyTwice (+3) to 10 = '.
	(applyTwice value: [:n | n + 3] value: 10) printNl.   "16"
]

"---- Selecting with a block predicate ----"
[
	Transcript nextPutAll: 'first even in #(3 5 8 9) = '.
	(#(3 5 8 9) detect: [:n | n even] ifNone: [nil]) printNl.
]

"---- Building an accumulator with inject:into: ----"
[
	| sum product |
	sum := (1 to: 10) inject: 0 into: [:acc :n | acc + n].
	product := (1 to: 5) inject: 1 into: [:acc :n | acc * n].
	Transcript nextPutAll: 'sum 1..10 = '. sum printNl.
	Transcript nextPutAll: '5! via inject = '. product printNl.
]
