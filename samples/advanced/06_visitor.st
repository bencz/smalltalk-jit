"============================================================
  ADVANCED - Visitor Pattern
  ------------------------------------------------------------
  Separate an algorithm from the object structure it runs on.
  Each node accepts a visitor and calls back the visit method
  for its own type (double dispatch). New operations become new
  visitors, without touching the node classes.

  Example: a small arithmetic-expression tree with two visitors,
  one that evaluates it and one that pretty-prints it.
  ============================================================"

"---- The object structure: expression nodes ----"
Expr := Object [
	accept: aVisitor [ ^self subclassResponsibility ]
]

Num := Expr [
	| value |
	class value: aNumber [ ^self new setValue: aNumber ]
	setValue: aNumber [ value := aNumber ]
	value [ ^value ]
	accept: aVisitor [ ^aVisitor visitNum: self ]
]

BinOp := Expr [
	| left right |
	setLeft: l right: r [ left := l. right := r ]
	left [ ^left ]
	right [ ^right ]
]

Add := BinOp [
	class left: l right: r [ ^self new setLeft: l right: r ]
	accept: aVisitor [ ^aVisitor visitAdd: self ]
]

Mul := BinOp [
	class left: l right: r [ ^self new setLeft: l right: r ]
	accept: aVisitor [ ^aVisitor visitMul: self ]
]

"---- Visitor 1: evaluate the expression to a number ----"
Evaluator := Object [
	visitNum: aNum [ ^aNum value ]
	visitAdd: anAdd [ ^(anAdd left accept: self) + (anAdd right accept: self) ]
	visitMul: aMul  [ ^(aMul left accept: self) * (aMul right accept: self) ]
]

"---- Visitor 2: render the expression as infix text ----"
Printer := Object [
	visitNum: aNum [ ^aNum value printString ]
	visitAdd: anAdd [ ^'(' , (anAdd left accept: self) , ' + ' , (anAdd right accept: self) , ')' ]
	visitMul: aMul  [ ^'(' , (aMul left accept: self) , ' * ' , (aMul right accept: self) , ')' ]
]

[
	| expr |
	"Build the tree for  (2 + 3) * (4 + 5)"
	expr := Mul
		left:  (Add left: (Num value: 2) right: (Num value: 3))
		right: (Add left: (Num value: 4) right: (Num value: 5)).

	Transcript nextPutAll: 'expression = '; nextPutAll: (expr accept: Printer new); lf.
	Transcript nextPutAll: 'value      = '; nextPutAll: (expr accept: Evaluator new) printString; lf.
]
