"============================================================
  10 - FizzBuzz
  ------------------------------------------------------------
  The interview classic: for 1..30 print 'Fizz' for multiples
  of 3, 'Buzz' for multiples of 5, 'FizzBuzz' for both, and the
  number otherwise.
  ============================================================"

[
	1 to: 30 do: [:n |
		| text |
		text := ''.
		(n \\ 3 = 0) ifTrue: [text := text , 'Fizz'].
		(n \\ 5 = 0) ifTrue: [text := text , 'Buzz'].
		text isEmpty ifTrue: [text := n printString].
		Transcript nextPutAll: text; lf].
]
