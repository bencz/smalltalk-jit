"============================================================
  ADVANCED - Decorator Pattern
  ------------------------------------------------------------
  Attach responsibilities to an object dynamically by wrapping
  it in another object that shares its interface and forwards
  to it, adding behaviour before/after.

  Example: a coffee shop. A base Beverage is wrapped in
  condiment decorators; each adds to the cost and the
  description. Prices are in cents to stay in integer land.
  ============================================================"

Beverage := Object [
	cost [ ^self subclassResponsibility ]
	description [ ^self subclassResponsibility ]
]

Espresso := Beverage [
	cost [ ^200 ]
	description [ ^'Espresso' ]
]

HouseBlend := Beverage [
	cost [ ^150 ]
	description [ ^'House Blend' ]
]

"A decorator IS-A Beverage and HAS-A wrapped Beverage."
CondimentDecorator := Beverage [
	| beverage |
	class on: aBeverage [ ^self new setBeverage: aBeverage ]
	setBeverage: aBeverage [ beverage := aBeverage ]
	beverage [ ^beverage ]
]

Milk := CondimentDecorator [
	cost [ ^beverage cost + 50 ]
	description [ ^beverage description , ' + Milk' ]
]

Mocha := CondimentDecorator [
	cost [ ^beverage cost + 75 ]
	description [ ^beverage description , ' + Mocha' ]
]

Whip := CondimentDecorator [
	cost [ ^beverage cost + 40 ]
	description [ ^beverage description , ' + Whip' ]
]

[
	| show order |
	show := [:drink |
		Transcript nextPutAll: '  '; nextPutAll: drink description;
			nextPutAll: '  = '; nextPutAll: drink cost printString;
			nextPutAll: ' cents'; lf].

	"Plain espresso."
	show value: Espresso new.

	"Espresso, wrapped in Mocha, then Whip."
	order := Whip on: (Mocha on: Espresso new).
	show value: order.

	"House blend with double mocha and milk."
	order := Milk on: (Mocha on: (Mocha on: HouseBlend new)).
	show value: order.
]
