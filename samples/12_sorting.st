"============================================================
  12 - Sorting Algorithms
  ------------------------------------------------------------
  Two hand-written sorts (bubble sort and quicksort), plus the
  built-in SortedCollection for comparison.
  ============================================================"

Sorter := Object [

	"In-place bubble sort on a copy of the input."
	bubbleSort: aCollection [
		| items n tmp swapped |
		items := aCollection asOrderedCollection.
		n := items size.
		swapped := true.
		[swapped] whileTrue: [
			swapped := false.
			1 to: n - 1 do: [:i |
				(items at: i) > (items at: i + 1) ifTrue: [
					tmp := items at: i.
					items at: i put: (items at: i + 1).
					items at: i + 1 put: tmp.
					swapped := true]]].
		^items
	]

	"Functional quicksort: pivot on the first element."
	quickSort: aCollection [
		| items pivot rest less greater |
		items := aCollection asOrderedCollection.
		items size <= 1 ifTrue: [^items].
		pivot := items first.
		rest := OrderedCollection new.
		2 to: items size do: [:i | rest add: (items at: i)].
		less    := rest select: [:x | x < pivot].
		greater := rest select: [:x | x >= pivot].
		^(self quickSort: less) , (OrderedCollection with: pivot) , (self quickSort: greater)
	]
]

[
	| sorter data |
	sorter := Sorter new.
	data := #(5 2 8 1 9 3 7 4 6).

	Transcript nextPutAll: 'input       = '. data printNl.
	Transcript nextPutAll: 'bubbleSort  = '. (sorter bubbleSort: data) printNl.
	Transcript nextPutAll: 'quickSort   = '. (sorter quickSort: data) printNl.
	Transcript nextPutAll: 'SortedColl  = '. data asSortedCollection printNl.
	Transcript nextPutAll: 'descending  = '.
		(data asSortedCollection: [:a :b | a >= b]) printNl.
]
