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


	"selector reflection"

	numArgs [
		"Number of arguments this symbol takes as a selector: 0 for a unary
		 selector, 1 for a binary selector, and the count of colons for a keyword
		 selector."
		| count |
		(self includes: $:) ifTrue: [
			count := 0.
			self do: [ :c | c = $: ifTrue: [count := count + 1]].
			^count].
		self isEmpty ifTrue: [^0].
		(self first isLetter or: [self first = $_]) ifTrue: [^0].
		^1
	]


	isUnary [
		"A unary selector takes no arguments (an identifier with no colon)."
		^self numArgs = 0
	]


	isBinary [
		"A binary selector is made of special characters and takes one argument."
		self isEmpty ifTrue: [^false].
		(self includes: $:) ifTrue: [^false].
		^(self first isLetter or: [self first = $_]) not
	]


	isKeywordSelector [
		"True when the symbol names a keyword message (contains a colon). Distinct
		 from String>>isKeyword, which only tests for a single trailing colon."
		^self includes: $:
	]


	"printing"

	printOn: aStream [
		aStream
			nextPut: $#;
			nextPutAll: self.
	]


	"displaying"

	displayString [
		"The characters without the leading # that printOn: adds."
		^self asString
	]


	displayOn: aStream [
		aStream nextPutAll: self.
	]

]
