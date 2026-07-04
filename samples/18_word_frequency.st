"============================================================
  18 - Word Frequency Counter
  ------------------------------------------------------------
  Tokenise a piece of text, tally how often each word appears
  in a Dictionary, then report the words ordered by frequency.
  ============================================================"

[
	| text words counts pairs |
	text := 'the cat sat on the mat then the cat saw the rat and the cat ran'.

	words := text splitBy: Character space.
	Transcript nextPutAll: 'total words = '. words size printNl.

	"Tally occurrences: at:ifAbsent: supplies a default of 0."
	counts := Dictionary new.
	words do: [:w |
		counts at: w put: (counts at: w ifAbsent: [0]) + 1].

	Transcript nextPutAll: 'distinct words = '. counts size printNl.

	"Sort the key->count associations by descending count."
	pairs := OrderedCollection new.
	counts associationsDo: [:assoc | pairs add: assoc].
	pairs := pairs asSortedCollection: [:a :b | a value >= b value].

	Transcript nextPutAll: 'frequencies:'; lf.
	pairs do: [:assoc |
		Transcript
			nextPutAll: '  '; nextPutAll: assoc key;
			nextPutAll: ' -> '; nextPutAll: assoc value printString; lf].
]
