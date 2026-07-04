"============================================================
  07 - Classes, Inheritance and Polymorphism
  ------------------------------------------------------------
  Class definition syntax in this VM:

      ClassName := SuperClass [
          | instanceVars |
          class instanceCreationMessage [ ... ]
          instanceMessage: arg [ ... ]
      ]

  We build an abstract Shape and three concrete subclasses,
  then treat them polymorphically.
  ============================================================"

"---- Abstract base class ----"
Shape := Object [
	name [ ^'shape' ]
	area [ ^self subclassResponsibility ]

	describe [
		Transcript
			nextPutAll: self name;
			nextPutAll: ' with area ';
			nextPutAll: self area printString;
			lf.
	]
]

"---- Circle ----"
Circle := Shape [
	| radius |
	class radius: r [ ^self new setRadius: r ]
	setRadius: r [ radius := r ]
	name [ ^'circle' ]
	area [ ^Float pi * radius * radius ]
]

"---- Rectangle ----"
Rectangle := Shape [
	| width height |
	class width: w height: h [ ^self new setWidth: w height: h ]
	setWidth: w height: h [ width := w. height := h ]
	name [ ^'rectangle' ]
	area [ ^width * height ]
]

"---- Square reuses Rectangle via super ----"
Square := Rectangle [
	class side: s [ ^self new setWidth: s height: s ]
	name [ ^'square' ]
]

"---- Polymorphism: one loop, many behaviours ----"
[
	| shapes total |
	shapes := OrderedCollection new.
	shapes add: (Circle radius: 2).
	shapes add: (Rectangle width: 3 height: 4).
	shapes add: (Square side: 5).

	shapes do: [:each | each describe].

	total := shapes inject: 0 into: [:sum :each | sum + each area].
	Transcript nextPutAll: 'total area = '. total printNl.
]

"============================================================
  A value object with operator methods and a custom printOn:
  ============================================================"

Vector2D := Object [
	| x y |
	class x: ax y: ay [ ^self new setX: ax y: ay ]
	setX: ax y: ay [ x := ax. y := ay ]
	x [ ^x ]
	y [ ^y ]

	+ other [ ^Vector2D x: x + other x y: y + other y ]
	* scalar [ ^Vector2D x: x * scalar y: y * scalar ]
	dot: other [ ^(x * other x) + (y * other y) ]
	length [ ^(self dot: self) sqrt ]

	"Custom textual representation, used by printNl and printString."
	printOn: aStream [
		aStream nextPut: $(.
		x printOn: aStream.
		aStream nextPutAll: ', '.
		y printOn: aStream.
		aStream nextPut: $).
	]
]

[
	| u v |
	u := Vector2D x: 1 y: 2.
	v := Vector2D x: 3 y: 4.
	Transcript nextPutAll: 'u       = '. u printNl.
	Transcript nextPutAll: 'u + v   = '. (u + v) printNl.
	Transcript nextPutAll: 'v * 2   = '. (v * 2) printNl.
	Transcript nextPutAll: 'u . v   = '. (u dot: v) printNl.
]
