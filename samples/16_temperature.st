"============================================================
  16 - Temperature Converter
  ------------------------------------------------------------
  Converts Celsius to Fahrenheit and Kelvin. Uses exact
  Fraction arithmetic internally and shows the Float result.
  ============================================================"

[
	| toF toK temps |
	toF := [:c | (c * 9 / 5) + 32].       "exact: may be a Fraction"
	toK := [:c | c + (5463 / 20)].         "273.15 as an exact Fraction"

	"Negative numbers are not allowed inside a #( ) literal, so we
	 build the list of sample temperatures explicitly."
	temps := OrderedCollection new.
	temps add: -40; add: 0; add: 20; add: 37; add: 100.

	Transcript nextPutAll: '   C          F          K'; lf.
	Transcript nextPutAll: '-----------------------------'; lf.

	temps do: [:c |
		| f k |
		f := (toF value: c) asFloat.
		k := (toK value: c) asFloat.
		Transcript
			nextPutAll: '  '; nextPutAll: c printString;
			tab; nextPutAll: f printString;
			tab; nextPutAll: k printString; lf].

	Transcript lf.
	Transcript nextPutAll: 'Note: 37 C -> '; nextPutAll: (toF value: 37) printString;
		nextPutAll: ' F exactly (= '; nextPutAll: (toF value: 37) asFloat printString;
		nextPutAll: ')'; lf.
]
