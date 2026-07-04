"============================================================
  ADVANCED - Composite Pattern
  ------------------------------------------------------------
  Compose objects into tree structures and treat individual
  objects (leaves) and compositions uniformly through a common
  protocol.

  Example: a file-system tree. A File is a leaf; a Directory is
  a composite holding other nodes. Both understand #size and
  #printOn:indent:, so client code never checks the type.
  ============================================================"

FSNode := Object [
	name [ ^self subclassResponsibility ]
	size [ ^self subclassResponsibility ]
	printOn: aStream indent: level [ ^self subclassResponsibility ]

	"Shared helper: write 'level' units of indentation."
	indent: level on: aStream [
		level timesRepeat: [aStream nextPutAll: '  '].
	]
]

File := FSNode [
	| name bytes |
	class named: aName size: aSize [ ^self new setName: aName size: aSize ]
	setName: aName size: aSize [ name := aName. bytes := aSize ]
	name [ ^name ]
	size [ ^bytes ]

	printOn: aStream indent: level [
		self indent: level on: aStream.
		aStream nextPutAll: name; nextPutAll: ' ('; nextPutAll: bytes printString;
			nextPutAll: ' bytes)'; lf.
	]
]

Directory := FSNode [
	| name children |
	class named: aName [ ^self new setName: aName ]
	setName: aName [ name := aName. children := OrderedCollection new ]
	name [ ^name ]

	add: aNode [ children add: aNode. ^aNode ]

	"A composite's size is the sum of its children's sizes."
	size [ ^children inject: 0 into: [:sum :child | sum + child size] ]

	printOn: aStream indent: level [
		self indent: level on: aStream.
		aStream nextPutAll: name; nextPutAll: '/  ('; nextPutAll: self size printString;
			nextPutAll: ' bytes total)'; lf.
		children do: [:child | child printOn: aStream indent: level + 1].
	]
]

[
	| root src |
	root := Directory named: 'project'.
	root add: (File named: 'README.md' size: 1200).

	src := Directory named: 'src'.
	src add: (File named: 'main.st' size: 4096).
	src add: (File named: 'utils.st' size: 2048).
	root add: src.

	root add: (File named: 'LICENSE' size: 1024).

	root printOn: Transcript indent: 0.
	Transcript nextPutAll: 'grand total = '; nextPutAll: root size printString;
		nextPutAll: ' bytes'; lf.
]
