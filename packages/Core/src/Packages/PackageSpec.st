"Package manifest object. A package.st file is ONE cascade expression that
 answers a PackageSpec (evaluated by PackageLoader through Compiler
 evaluate:); it is data, not a class definition, so a build leaves no
 residue in the image. Dependencies are declared BY NAME (requires:) and
 resolved against the package roots (PackageLoader packageRoots);
 requires:path: is the local-development override. requires:version: is an
 exact-match check in v1; semver ranges arrive with the future registry."
PackageSpec := Object [

	| name version summary requirements files testFiles entryClassName entrySelector rootPath allowUnresolved |


	class new [
		^self basicNew init
	]


	init [
		requirements := OrderedCollection new.
		files := Array new: 0.
		testFiles := Array new: 0.
		allowUnresolved := Array new: 0.
		entrySelector := #main:.
	]


	"manifest DSL"

	name: aString [
		name := aString.
	]


	version: aString [
		version := aString.
	]


	summary: aString [
		summary := aString.
	]


	requires: aName [
		requirements add: (PackageRequirement name: aName path: nil version: nil).
	]


	requires: aName path: aPath [
		requirements add: (PackageRequirement name: aName path: aPath version: nil).
	]


	requires: aName version: aVersion [
		requirements add: (PackageRequirement name: aName path: nil version: aVersion).
	]


	files: anArray [
		files := anArray.
	]


	testFiles: anArray [
		testFiles := anArray.
	]


	entry: aSymbol [
		entryClassName := aSymbol.
	]


	entry: aSymbol selector: aSelector [
		entryClassName := aSymbol.
		entrySelector := aSelector.
	]


	allowUnresolved: anArray [
		allowUnresolved := anArray.
	]


	"accessing"

	name [
		^name
	]


	version [
		^version
	]


	summary [
		^summary
	]


	requirements [
		^requirements
	]


	files [
		^files
	]


	testFiles [
		^testFiles
	]


	entryClassName [
		^entryClassName
	]


	entrySelector [
		^entrySelector
	]


	allowUnresolved [
		^allowUnresolved
	]


	rootPath [
		^rootPath
	]


	rootPath: aString [
		rootPath := aString.
	]


	isApplication [
		^entryClassName notNil
	]


	validateFrom: aManifestPath [
		name isNil ifTrue: [Error signal: 'manifest missing name: (', aManifestPath, ')'].
	]


	printOn: aStream [
		aStream nextPutAll: 'PackageSpec('.
		name isNil ifFalse: [aStream nextPutAll: name].
		aStream nextPutAll: ')'.
	]

]
