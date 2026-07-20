Object := nil [

	"accessing"

	at: anInteger [
		<primitive: AtPrimitive>
		(OutOfRangeError value: anInteger between: 1 and: self basicSize) signal.
	]


	at: anInteger put: anObject [
		<primitive: AtPutPrimitive>
		(OutOfRangeError value: anInteger between: 1 and: self basicSize) signal.
	]


	basicAt: anInteger [
		<primitive: AtPrimitive>
		(OutOfRangeError value: anInteger between: 1 and: self basicSize) signal.
	]


	basicAt: anInteger put: anObject [
		<primitive: AtPutPrimitive>
		(OutOfRangeError value: anInteger between: 1 and: self basicSize) signal.
	]


	size [
		<primitive: SizePrimitive>
	]


	basicSize [
		<primitive: SizePrimitive>
	]


	yourself [
		^self
	]


	"nil testing (non-nil branch; UndefinedObject overrides these)"

	ifNil: aBlock [
		^self
	]


	ifNotNil: aBlock [
		^aBlock value: self
	]


	ifNil: nilBlock ifNotNil: notNilBlock [
		^notNilBlock value: self
	]


	ifNotNil: notNilBlock ifNil: nilBlock [
		^notNilBlock value: self
	]


	"error signalling"

	subclassResponsibility [
		^SubClassResponsibility signal
	]


	shouldNotImplement [
		^ShouldNotImplement signal
	]


	"system primitives"

	instVarAt: anInteger [
		<primitive: InstVarAtPrimitive>
		(OutOfRangeError value: anInteger between: 1 and: self class instanceSize) signal.
	]


	instVarAt: anInteger put: anObject [
		<primitive: InstVarAtPutPrimitive>
		(OutOfRangeError value: anInteger between: 1 and: self class instanceSize) signal.
	]


	become: otherObject [
		<primitive: BecomePrimitive>
	]


	"comparing"

	= anObject [
		<primitive: IdentityPrimitive>
	]


	== anObject [
		<primitive: IdentityPrimitive>
	]


	hash [
		<primitive: HashPrimitive>
	]


	~= anObject [
		^self = anObject == false
	]


	~~ anObject [
		^self == anObject == false
	]


	identityHash [
		<primitive: HashPrimitive>
	]


	"private"

	species [
		^self class
	]


	"class membership"

	class [
		<primitive: ClassPrimitive>
	]


	isKindOf: aClass [
		^self class == aClass
			ifTrue: [true]
			ifFalse: [self class inheritsFrom: aClass]
	]


	isMemberOf: aClass [
		^self class == aClass
	]


	respondsTo: aSymbol [
		^self class canUnderstand: aSymbol
	]


	"testing"

	isNil [
		^false
	]


	notNil [
		^self isNil not
	]


	"copying"

	copy [
		^self shallowCopy
	]


	shallowCopy [
		| copy class index |

		class := self class.

		class isIndexable
			ifTrue: [
				index := self basicSize.
				copy := class basicNew: index.
				[index > 0] whileTrue: [
					copy basicAt: index put: (self basicAt: index).
					index := index - 1]]
			ifFalse: [
				copy := class basicNew].

		index := class instanceSize.
		[index > 0] whileTrue: [
			copy instVarAt: index put: (self instVarAt: index).
			index := index - 1].

		^copy
	]


	deepCopy [
		| copy class index |

		class := self class.

		class isIndexable
			ifTrue: [
				index := self basicSize.
				copy := class basicNew: index.
				[index > 0] whileTrue: [
					copy basicAt: index put: (self basicAt: index) copy.
					index := index - 1]]
			ifFalse: [
				copy := class basicNew].

		index := class instanceSize.
		[index > 0] whileTrue: [
			copy instVarAt: index put: (self instVarAt: index) copy.
			index := index - 1].

		^copy
	]


	"message sending"

	perform: aSymbol [
		^(self class lookupSelector: aSymbol) sendTo: self
	]


	perform: aSymbol with: anObject [
		^(self class lookupSelector: aSymbol) sendTo: self withArguments: (Array with: anObject)
	]


	perform: aSymbol with: anObject with: anObject2 [
		^(self class lookupSelector: aSymbol) sendTo: self
			withArguments: (Array with: anObject with: anObject2)
	]


	perform: aSymbol withArguments: anArray [
		^(self class lookupSelector: aSymbol) sendTo: self withArguments: anArray
	]


	"error handling"

	error: aString [
		^Error signal: aString
	]


	assert: aBlockOrBoolean [
		"Evaluate aBlockOrBoolean (a Block is sent #value; a Boolean is taken as
		 is) and raise an AssertError unless the result is true. Answer self on
		 success."
		| ok |
		ok := (aBlockOrBoolean isKindOf: Block)
			ifTrue: [aBlockOrBoolean value]
			ifFalse: [aBlockOrBoolean].
		ok == true ifFalse: [^AssertError expected: true got: ok].
		^self
	]


	mustBeBoolean [
		"Sent by inlined conditionals (ifTrue:/and:/...) when the receiver is
		 neither true nor false."
		^self error: 'NonBoolean receiver: does not understand a Boolean message'
	]


	doesNotUnderstand: aMessage [
		MessageNotUnderstood new
			receiver: self;
			message: aMessage;
			signal.
	]


	"associating"

	-> value [
		^Association key: self value: value
	]


	"printing"

	printString [
		| stream |

		stream := CollectionStream with: (String new: 128).
		self printOn: stream.
		^stream contents
	]


	printNl [
		self printOn: Transcript.
		Transcript lf.
	]


	print [
		self printOn: Transcript.
	]


	"displaying"

	displayString [
		"A user-facing string form. Defaults to printString; String, Symbol and
		 Character override it to drop the printOn: decoration (quotes, #, $)."
		^self printString
	]


	displayOn: aStream [
		self printOn: aStream.
	]


	displayNl [
		"Print displayString followed by a newline on the Transcript; answer self.
		 Mirrors printNl but uses the display (undecorated) form."
		Transcript nextPutAll: self displayString.
		Transcript lf.
		^self
	]


	printOn: aStream [
		self basicPrintOn: aStream.
	]


	basicPrintOn: aStream [
		| name |

		name := self class name.
		aStream
			nextPutAll: (name first isVowel ifTrue: ['an '] ifFalse: ['a ']);
			nextPutAll: name.
	]


	inspect [
		self examineOn: Transcript.
	]


	examineOn: aStream [
		| instVars |

		instVars := self class instanceVariables.

		self basicPrintOn: aStream.
		aStream lf.

		1 to: instVars size do: [ :i |
			aStream
				space;
				nextPutAll: (instVars at: i);
				nextPutAll: ': '.
			(self instVarAt: i) == self
				ifTrue: [aStream nextPutAll: 'self (recursion)']
				ifFalse: [(self instVarAt: i) basicPrintOn: aStream].
			aStream lf].

		1 to: self basicSize do: [ :i |
			aStream nextPutAll: ' - '.
			(self basicAt: i) == self
				ifTrue: [aStream nextPutAll: 'self (recursion)']
				ifFalse: [(self basicAt: i) basicPrintOn: aStream].
			aStream lf].
	]

]
