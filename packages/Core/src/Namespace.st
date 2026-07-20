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
