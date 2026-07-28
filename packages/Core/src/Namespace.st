"Mirror of the VM-known RawNamespace (vm/core/Namespace.h). One per loaded
 package. The Core namespace wraps the Smalltalk system dictionary itself:
 (Namespaces at: #Core) bindings == Smalltalk. Name resolution at compile time
 walks: own bindings, then imports in declaration order, then Core."
Namespace := Object [

	| name bindings imports |


	"instance creation"

	class named: aSymbol imports: anArray [
		| ns |
		ns := self new.
		ns setName: aSymbol bindings: Dictionary new imports: anArray.
		^ns
	]


	"session default: what eval and class builds without an explicit
	 namespace compile against. Core until a project image changes it."

	class default [
		<primitive: DefaultNamespacePrimitive>
	]


	class default: aNamespace [
		<primitive: SetDefaultNamespacePrimitive>
		InvalidArgumentError signal: 'default: expects a Namespace'.
	]


	"initialization"

	class initialize [
		"Namespaces, the registry name -> Namespace, and Core in it.

		 It is built HERE and not in the VM because a Namespace is an ordinary
		 Smalltalk object of a class this file defines, and the C bootstrap runs
		 before this file is loaded. What the VM does provide is the identity the
		 Core namespace is made of: `Smalltalk` is the system dictionary itself,
		 registered under that name by tools/Bootstrap.c.

		 CORE'S BINDINGS *ARE* SMALLTALK, the same Dictionary object and not a
		 copy. That is the invariant the whole scheme rests on: it keeps every
		 pre-namespace path -- `Smalltalk at:`, the C getGlobal/setGlobal family,
		 the bootstrap -- meaning exactly the Core namespace, and it is what
		 core/Namespace.c tests to recognise Core without trusting a name."
		| core |

		Namespaces := Dictionary new.
		core := self new.
		core setName: #Core bindings: Smalltalk imports: (Array new: 0).
		Namespaces at: #Core put: core.
	]


	setName: aSymbol bindings: aDictionary imports: anArray [
		name := aSymbol.
		bindings := aDictionary.
		imports := anArray.
	]


	"accessing"

	name [
		^name
	]


	bindings [
		^bindings
	]


	imports [
		^imports
	]


	at: aSymbol [
		^bindings at: aSymbol
	]


	at: aSymbol ifAbsent: aBlock [
		^bindings at: aSymbol ifAbsent: aBlock
	]


	at: aSymbol put: anObject [
		^bindings at: aSymbol put: anObject
	]


	includesKey: aSymbol [
		^bindings includesKey: aSymbol
	]


	"Bindings auto-created for a forward reference and never assigned a value:
	 after a package finishes loading (including class initializers), survivors
	 are typos or missing definitions and the loader reports them."
	unresolvedNames [
		| names |
		names := OrderedCollection new.
		bindings associationsDo: [ :assoc | assoc value isNil ifTrue: [names add: assoc key]].
		^names
	]


	"printing"

	printOn: aStream [
		aStream nextPutAll: 'Namespace '.
		aStream nextPutAll: name.
	]

]
