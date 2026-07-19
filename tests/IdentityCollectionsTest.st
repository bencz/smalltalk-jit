"IdentityCollectionsTest: IdentitySet and IdentityDictionary compare by
 identity (==) where Set and Dictionary compare by value (=). Two value-equal
 but distinct String objects (built with copyFrom: so they are never the same
 interned literal) stay separate in the identity collections and collapse in the
 value collections; identical Symbols collapse in both."

[
	| t a b s is d id sym syms symd |
	t := TestRun named: 'IdentityCollections'.

	"distinct objects that are value-equal"
	a := 'zzab' copyFrom: 3 to: 4.
	b := 'qqab' copyFrom: 3 to: 4.
	t check: 'strings are value-equal' that: [ a = b ].
	t check: 'strings are not identical' that: [ (a == b) not ].

	"---- Set collapses value-equal, IdentitySet keeps them apart ----"
	s := Set new. s add: a; add: b.
	t check: 'Set collapses value-equal strings' equals: 1 is: [ s size ].
	is := IdentitySet new. is add: a; add: b.
	t check: 'IdentitySet keeps distinct objects' equals: 2 is: [ is size ].
	t check: 'IdentitySet finds the exact object' that: [ is includes: a ].
	t check: 'IdentitySet rejects a value-equal stranger' that: [
		(is includes: ('ppab' copyFrom: 3 to: 4)) not ].

	"---- Dictionary collapses value-equal keys, IdentityDictionary keeps them apart ----"
	d := Dictionary new. d at: a put: 1; at: b put: 2.
	t check: 'Dictionary collapses value-equal keys' equals: 1 is: [ d size ].
	t check: 'Dictionary last write wins' equals: 2 is: [ d at: a ].
	id := IdentityDictionary new. id at: a put: 1; at: b put: 2.
	t check: 'IdentityDictionary keeps distinct keys' equals: 2 is: [ id size ].
	t check: 'IdentityDictionary value for a' equals: 1 is: [ id at: a ].
	t check: 'IdentityDictionary value for b' equals: 2 is: [ id at: b ].

	"---- identical Symbols collapse in the identity collections ----"
	sym := #wave2symbol.
	syms := IdentitySet new. syms add: sym; add: #wave2symbol.
	t check: 'IdentitySet collapses identical Symbols' equals: 1 is: [ syms size ].
	symd := IdentityDictionary new. symd at: #wave2symbol put: 1; at: #wave2symbol put: 2.
	t check: 'IdentityDictionary collapses identical Symbols' equals: 1 is: [ symd size ].
	t check: 'IdentityDictionary Symbol last write wins' equals: 2 is: [ symd at: #wave2symbol ].

	^t report
]
