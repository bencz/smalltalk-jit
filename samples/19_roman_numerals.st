"============================================================
  19 - Roman Numerals
  ------------------------------------------------------------
  Convert integers to Roman numerals with the classic greedy
  algorithm, driven by an ordered table of value->symbol pairs
  (Associations, created with the -> message).
  ============================================================"

Roman := Object [

	table [
		"Largest first, including the subtractive forms (CM, CD, XC ...)."
		^OrderedCollection new
			add: 1000 -> 'M';  add: 900 -> 'CM'; add: 500 -> 'D'; add: 400 -> 'CD';
			add:  100 -> 'C';  add:  90 -> 'XC'; add:  50 -> 'L'; add:  40 -> 'XL';
			add:   10 -> 'X';  add:   9 -> 'IX'; add:   5 -> 'V'; add:   4 -> 'IV';
			add:    1 -> 'I';  yourself
	]

	toRoman: anInteger [
		| remaining result |
		remaining := anInteger.
		result := ''.
		self table do: [:pair |
			[remaining >= pair key] whileTrue: [
				result := result , pair value.
				remaining := remaining - pair key]].
		^result
	]
]

[
	| roman |
	roman := Roman new.
	#(4 9 14 40 90 444 1994 2024 3888) do: [:n |
		Transcript
			nextPutAll: '  '; nextPutAll: n printString;
			tab; nextPutAll: (roman toRoman: n); lf].
]
