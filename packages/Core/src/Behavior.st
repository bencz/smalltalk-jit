Behavior := Object [

	"THE VM OWNS THIS LAYOUT. These five names mirror, in order, the tagged
	 fields of RawClass in vm/core/Object.h, and the class builder refuses to
	 build the mirror if the two stop agreeing.

	 `instanceShape` is NOT among them any more. In jit-v2 the shape is a RAW
	 word in the class's trailer rather than a tagged slot, precisely so the
	 collector never walks it (ADR 0005), so Smalltalk reaches it through a
	 primitive instead of through a field."

	<shape: ClassShape>

	| superClass subClasses methodDictionary instanceVariables |


	"instance creation"

	new [
		<primitive: BehaviorNewPrimitive>
	]


	new: anInteger [
		<primitive: BehaviorNewSizePrimitive>
		(anInteger isMemberOf: SmallInteger) ifFalse: [Error signal: 'size must be SmallInteger'].
		Error signal.
	]


	basicNew [
		<primitive: BehaviorNewPrimitive>
	]


	basicNew: anInteger [
		<primitive: BehaviorNewSizePrimitive>
		Error signal.
	]


	"shape and size"

	instanceSize [
		"Named instance variables. The `fixedSlots` field of the shape word."
		^(self instanceShape bitShift: -24) bitAnd: 16rFFFF
	]


	instanceShape [
		"The packed shape word out of the class's raw trailer. A primitive
		 because it is not a tagged field: see the note at the top.

		 THE LAYOUT IS THE CONTRACT with primInstanceShape (vm/runtime/
		 Primitive.c), which is the only other place it is written:

		    bits  0..7   format
		    bits  8..15  rawWords
		    bits 16..23  pointerWords
		    bits 24..39  fixedSlots

		 This is the jit-v2 shape and it is NOT the one this file used to
		 decode. The old fields were at other offsets entirely, so isIndexable
		 answered false for every class in the system, including Array, and the
		 damage was silent and wide: shallowCopy takes the not-indexed branch,
		 so `#(1 2 3) copy` came back an EMPTY Array, and everything built on
		 copy (sort, replaceAll:, replaceFrom:to:with:, Dictionary copy) got an
		 empty collection and reported a plain wrong answer rather than failing."
		<primitive: BehaviorInstanceShapePrimitive>
		^0
	]


	instanceFormat [
		"The ObjectFormat of instances: 0 no pointers, 1 pointers, 2 indexed
		 pointers, 3 bytes, 4 doubles, 5 mixed pointers, 6 mixed bytes."
		^self instanceShape bitAnd: 16rFF
	]


	isIndexable [
		"Exactly the three formats whose basicSize is an element count and whose
		 basicAt: is an element accessor, which is what every caller here means:
		 indexed pointers, bytes and doubles. The MIXED formats are deliberately
		 out, and that agrees with the size primitive, which refuses them."
		| format |

		format := self instanceFormat.
		^format = 2 or: [format = 3 or: [format = 4]]
	]


	instanceVariables [
		"EVERY named variable of an instance, INHERITED FIRST, as an Array of
		 Strings.

		 THE INDICES LINE UP WITH instVarAt:, and that is the contract the rest
		 of the kernel is written against: `Json class encodeObject:` pairs
		 `(names at: i)` with `(anObject instVarAt: i)`, `Object>>basicPrintOn:`
		 does the same, and `Object>>copy` walks 1 to `instanceSize`. So
		 `self instanceVariables size = self instanceSize` has to hold, and it
		 only does if the inherited ones are here and come first.

		 THE FIELD HOLDS ONLY WHAT THIS CLASS DECLARES, and the chain is composed
		 here rather than stored: the field is the working representation the
		 compiler's slot numbering walks (vm/compiler/Compile.c, indexOfSymbol),
		 which repeats nothing, and storing the full list would number every
		 inherited variable twice.

		 A FRESH ARRAY OF STRINGS, not the field: handing back the collection
		 that decides where every instance variable lives means one `add:` away
		 from instances numbered differently from the methods already compiled
		 against them."
		| names |

		names := superClass isNil
			ifTrue: [OrderedCollection new]
			ifFalse: [superClass instanceVariables asOrderedCollection].
		instanceVariables isNil ifFalse: [
			instanceVariables do: [ :each | names add: each asString]].
		^names asArray
	]


	"accessing class hierarchy"

	superClass [
		^superClass
	]


	subClasses [
		"The DIRECT subclasses. The class builder appends to this field when it
		 builds a class, so NIL MEANS NO SUBCLASSES YET and is answered as an
		 empty collection rather than as nil.

		 It is not stored empty: that would allocate an OrderedCollection for
		 every leaf class in the image, and every one of the 158 kernel classes
		 pays for it at bootstrap. Answering the empty case here is what makes
		 allSubClassesDo: below terminate at a leaf instead of sending #do: to
		 nil."
		^subClasses ifNil: [OrderedCollection new]
	]


	methodDictionary [
		^methodDictionary
	]


	removeSelector: aSymbol [
		"Remove aSymbol from the receiver's OWN method dictionary, under a
		 full send-cache invalidation (dev reload; the additive counterpart is
		 the `Name extend [ ... ]` syntax). Signals when the selector is not
		 defined here."
		<primitive: ClassRemoveSelectorPrimitive>
		(NotFoundError value: aSymbol) signal
	]


	"testing class hierarchy"

	inheritsFrom: aClass [
		| aSuperClass |

		aSuperClass := superClass.
		[aSuperClass == nil] whileFalse: [
			aSuperClass == aClass ifTrue: [^true].
			aSuperClass := aSuperClass superClass].
		^false
	]


	"accessing the method dictionary"

	selectors [
		^methodDictionary keys
	]


	allSelectors [
		| selectors |

		selectors := self selectors.
		self allSuperClassesDo: [ :class | selectors addAll: class selectors].
		^selectors
	]


	lookupSelector: aSymbol [
		"The method this class would run for aSymbol, or nil. The primitive is
		 the same superclass-chain walk in C; this body is its fallback for a
		 selector that is neither Symbol nor String, and both answer nil when
		 nothing in the chain implements it."
		<primitive: BehaviorLookupSelectorPrimitive>
		| class |

		class := self.
		[class methodDictionary at: aSymbol ifPresent: [ :code | ^code].
		(class := class superClass) isNil] whileFalse.
		^nil
	]


	compiledMethodAt: aSymbol [
		^methodDictionary at: aSymbol
	]


	"testing method dictionary"

	canUnderstand: aSymbol [
		(self includesSelector: aSymbol) ifTrue: [^true].
		superClass == nil ifTrue: [^false].
		^superClass canUnderstand: aSymbol
	]


	includesSelector: aSymbol [
		^methodDictionary includesKey: aSymbol
	]


	"enumerating"

	allSuperClassesDo: aBlock [
		| class |

		class := self.
		[(class := class superClass) notNil] whileTrue: [
			aBlock value: class].
	]


	allSubClassesDo: aBlock [
		self subClasses do: [ :aClass |
			aBlock value: aClass.
			aClass allSubClassesDo: aBlock].
	]


	"printing"

	printHierarchy [
		Transcript nextPutAll: self name; lf.
		self printSubClassesOn: Transcript level: 1.
	]


	printSubClassesOn: aStream level: anInteger [
		self subClasses do: [ :aClass |
			Transcript
				next: anInteger put: Character space;
				nextPutAll: aClass name;
				lf.
			aClass printSubClassesOn: aStream level: anInteger + 1].
	]

]
