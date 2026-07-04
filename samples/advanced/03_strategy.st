"============================================================
  ADVANCED - Strategy Pattern
  ------------------------------------------------------------
  A Strategy encapsulates an interchangeable algorithm. In
  Smalltalk a block is the most natural strategy object, so we
  show both flavours:

    1. blocks as strategies (the idiomatic way)
    2. strategy objects with a common protocol

  The example: a shopping Cart whose total depends on a pluggable
  discount strategy.
  ============================================================"

Cart := Object [
	| items discount |

	class new [ ^super new initCart ]
	initCart [ items := OrderedCollection new. discount := [:subtotal | subtotal] ]

	add: price [ items add: price ]
	discountStrategy: aBlock [ discount := aBlock ]

	subtotal [ ^items inject: 0 into: [:sum :p | sum + p] ]

	total [ ^discount value: self subtotal ]
]

"Strategy objects: each answers discountOn: to keep a uniform protocol."
NoDiscount := Object [
	discountOn: subtotal [ ^subtotal ]
	name [ ^'none' ]
]

PercentOff := Object [
	| percent |
	class percent: p [ ^self new setPercent: p ]
	setPercent: p [ percent := p ]
	discountOn: subtotal [ ^subtotal - (subtotal * percent // 100) ]
	name [ ^percent printString , '% off' ]
]

[
	| cart |
	cart := Cart new.
	cart add: 40; add: 35; add: 25.       "subtotal = 100"
	Transcript nextPutAll: 'subtotal = '. cart subtotal printNl.

	"---- Blocks as strategies ----"
	cart discountStrategy: [:s | s].
	Transcript nextPutAll: 'no discount        -> '. cart total printNl.

	cart discountStrategy: [:s | s - 15].
	Transcript nextPutAll: 'minus 15 flat      -> '. cart total printNl.

	cart discountStrategy: [:s | s * 9 // 10].
	Transcript nextPutAll: '10% off (block)    -> '. cart total printNl.
]

[
	| cart strategies |
	cart := Cart new.
	cart add: 40; add: 35; add: 25.

	"---- Strategy objects sharing the discountOn: protocol ----"
	strategies := OrderedCollection new.
	strategies add: NoDiscount new; add: (PercentOff percent: 20); add: (PercentOff percent: 50).
	strategies do: [:strategy |
		cart discountStrategy: [:s | strategy discountOn: s].
		Transcript nextPutAll: 'strategy '; nextPutAll: strategy name;
			nextPutAll: ' -> '; nextPutAll: cart total printString; lf].
]
