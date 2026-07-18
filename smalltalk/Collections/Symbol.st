Symbol := String [

	"comparing"

	hash [
		<primitive: HashPrimitive>
	]


	= anObject [
		^self == anObject
	]


	"Symbols are interned and shared: an in-place store would corrupt every other
	 reference to the same symbol and the global symbol table, so they are
	 immutable. String operations that would build a new sequence answer a String
	 (species = String), the way they cannot be produced by mutating a symbol."
	species [
		^String
	]


	at: index put: aValue [
		^self shouldNotImplement
	]


	"converting"

	asString [
		"Answer a plain, mutable String with the same characters (no leading #).
		 copyFrom: builds a String because Symbol species is String."
		^self copyFrom: 1 to: self size
	]

	asSymbol [

	]


	"printing"

	printOn: aStream [
		aStream
			nextPut: $#;
			nextPutAll: self.
	]

]
