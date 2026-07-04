"============================================================
  03 - Control Flow
  ------------------------------------------------------------
  In Smalltalk there are no keywords for control flow. 'if',
  'while' and 'for' are just ordinary messages sent to booleans,
  blocks and numbers.
  ============================================================"

"---- Conditionals ----"
[
	| n |
	n := 7.
	Transcript nextPutAll: 'n = '. n printNl.

	(n even)
		ifTrue:  [Transcript nextPutAll: 'even'; lf]
		ifFalse: [Transcript nextPutAll: 'odd'; lf].

	"ifTrue:ifFalse: returns a value, so it can be used as an expression."
	Transcript nextPutAll: 'sign word: '.
	(n > 0 ifTrue: ['positive'] ifFalse: ['non-positive']) printNl.
]

"---- Boolean logic (& | and: or: not) ----"
[
	| age hasTicket |
	age := 20.
	hasTicket := true.

	"and:/or: take a block and short-circuit; & and | are eager."
	Transcript nextPutAll: 'may enter: '.
	((age >= 18) and: [hasTicket]) printNl.
	Transcript nextPutAll: 'not true = '.
	true not printNl.
]

"---- while loops ----"
[
	| i |
	i := 1.
	Transcript nextPutAll: 'countdown: '.
	[i <= 5] whileTrue: [
		Transcript nextPutAll: i printString; space.
		i := i + 1].
	Transcript lf.
]

"---- counted loops: to:do: and to:by:do: ----"
[
	Transcript nextPutAll: 'squares: '.
	1 to: 6 do: [:i | Transcript nextPutAll: (i * i) printString; space].
	Transcript lf.

	Transcript nextPutAll: 'even down: '.
	10 to: 2 by: -2 do: [:i | Transcript nextPutAll: i printString; space].
	Transcript lf.
]

"---- timesRepeat: ----"
[
	| stars |
	stars := ''.
	5 timesRepeat: [stars := stars , '*'].
	Transcript nextPutAll: stars; lf.
]

"---- FizzBuzz-style dispatch with a cascade of conditions ----"
[
	Transcript nextPutAll: 'grades: '; lf.
	#(95 82 74 61 40) do: [:score |
		| grade |
		grade := score >= 90 ifTrue: ['A'] ifFalse: [
			score >= 80 ifTrue: ['B'] ifFalse: [
				score >= 70 ifTrue: ['C'] ifFalse: [
					score >= 60 ifTrue: ['D'] ifFalse: ['F']]]].
		Transcript nextPutAll: '  '; nextPutAll: score printString;
			nextPutAll: ' -> '; nextPutAll: grade; lf].
]
