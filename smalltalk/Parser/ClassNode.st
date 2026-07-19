ClassNode := Object [

	| name superName pragmas variables methods sourceCode isExtension members |


	name [
		^name
	]


	isExtension [
		"true for `Name extend [ ... ]` nodes: the build adds methods to the
		 existing class instead of defining a new one, and loaders must not
		 re-send initialize to the extended class."
		^isExtension == true
	]


	isNamespace [
		"true for `Name := Namespace [ ... ]` declarations: members holds the
		 enclosed class nodes. Loaders must not re-send initialize (the
		 declaration initializes its own members)."
		^members notNil
	]


	members [
		^members
	]


	superName [
		^superName
	]


	variables [
		^variables
	]


	methods [
		^methods
	]


	sourceCode [
		^sourceCode
	]


	acceptVisitor: aVisitor [
		aVisitor visitClassNode: self.
		name acceptVisitor: aVisitor.
		superName acceptVisitor: aVisitor.
		pragmas do: [ :pragma | pragma acceptVisitor: aVisitor].
		variables do: [ :var | var acceptVisitor: aVisitor].
		methods do: [ :method | method acceptVisitor: aVisitor].
	]

]
