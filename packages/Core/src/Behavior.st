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
		^(self instanceShape bitShift: -14) bitAnd: 16rFF
	]


	instanceShape [
		"The packed shape word out of the class's raw trailer. A primitive
		 because it is not a tagged field: see the note at the top."
		<primitive: BehaviorInstanceShapePrimitive>
		^0
	]


	isIndexable [
		^((self instanceShape bitShift: -22) bitAnd: 16rFF) = 1
	]


	instanceVariables [
		^instanceVariables isNil ifTrue: [#()] ifFalse: [instanceVariables]
	]


	"accessing class hierarchy"

	superClass [
		^superClass
	]


	subClasses [
		^subClasses
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
