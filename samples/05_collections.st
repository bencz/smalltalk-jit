"============================================================
  05 - Collections
  ------------------------------------------------------------
  A tour of the collection classes and the enumeration protocol
  (do: collect: select: reject: detect: inject:into:) shared by
  all of them.
  ============================================================"

"---- Array: fixed size, literal syntax #( ... ) ----"
[
	| a |
	a := #(10 20 30 40 50).
	Transcript nextPutAll: 'array         = '. a printNl.
	Transcript nextPutAll: 'size          = '. a size printNl.
	Transcript nextPutAll: 'third element = '. (a at: 3) printNl.
	Transcript nextPutAll: 'includes 30   = '. (a includes: 30) printNl.
]

"---- Enumeration: collect / select / reject ----"
[
	| nums |
	nums := 1 to: 10.
	Transcript nextPutAll: 'doubled = '. (nums collect: [:n | n * 2]) printNl.
	Transcript nextPutAll: 'evens   = '. (nums select: [:n | n even]) printNl.
	Transcript nextPutAll: 'not <5  = '. (nums reject: [:n | n < 5]) printNl.
	Transcript nextPutAll: 'sum     = '. (nums inject: 0 into: [:a :b | a + b]) printNl.
	Transcript nextPutAll: 'first>5 = '. (nums detect: [:n | n > 5] ifNone: [0]) printNl.
]

"---- OrderedCollection: growable, like a dynamic array ----"
[
	| oc |
	oc := OrderedCollection new.
	oc add: 'b'; add: 'c'; addFirst: 'a'; addLast: 'd'.
	Transcript nextPutAll: 'ordered   = '. oc printNl.
	oc removeFirst.
	oc removeLast.
	Transcript nextPutAll: 'trimmed   = '. oc printNl.
	Transcript nextPutAll: 'concat    = '. (oc , (OrderedCollection with: 'z')) printNl.
]

"---- Set: no duplicates ----"
[
	| s |
	s := Set new.
	#(1 2 2 3 3 3 4) do: [:each | s add: each].
	Transcript nextPutAll: 'set size (from 1 2 2 3 3 3 4) = '. s size printNl.
]

"---- Bag: counts occurrences ----"
[
	| b |
	b := Bag new.
	'mississippi' do: [:ch | b add: ch].
	Transcript nextPutAll: 'letter s occurs '. (b occurrencesOf: $s) printNl.
	Transcript nextPutAll: 'letter i occurs '. (b occurrencesOf: $i) printNl.
]

"---- Dictionary: key -> value associations ----"
[
	| ages |
	ages := Dictionary new.
	ages at: 'Alice' put: 30.
	ages at: 'Bob'   put: 25.
	ages at: 'Carol' put: 35.
	Transcript nextPutAll: 'Bob is '. (ages at: 'Bob') printNl.
	Transcript nextPutAll: 'has Dave? '. (ages includesKey: 'Dave') printNl.
	Transcript nextPutAll: 'default:  '. (ages at: 'Dave' ifAbsent: [0]) printNl.

	Transcript nextPutAll: 'everyone:'; lf.
	ages keysAndValuesDo: [:name :age |
		Transcript nextPutAll: '  '; nextPutAll: name;
			nextPutAll: ' is '; nextPutAll: age printString; lf].
]
