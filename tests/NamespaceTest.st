"Real namespaces: per-package name universes with core visible everywhere.
 Covers: registry and Core identity, building classes into a namespace,
 resolution order (own > imports > core), shadowing a core name without
 touching core, independence of same-named classes across namespaces,
 compile-time binding stability, in-package forward references (vivify into
 the ACTIVE namespace, never core), unresolvedNames, and the session default
 namespace driving eval."

NsTestHelper := Object [

	class build: aSourceString in: aNamespace [
		^Compiler new buildClass: (Parser parseString: aSourceString) parseClass in: aNamespace
	]

	class newNamespace: aSymbol imports: anArray [
		| ns |
		ns := Namespace named: aSymbol imports: anArray.
		Namespaces at: aSymbol put: ns.
		^ns
	]

]

[ | t nsA nsB coreOrdColl |
	t := TestRun named: 'Namespace'.

	t check: 'Core namespace wraps the system dictionary (same identity)'
		that: [ (Namespaces at: #Core) bindings == Smalltalk ].
	t check: 'the session default starts as Core'
		that: [ Namespace default == (Namespaces at: #Core) ].

	nsA := NsTestHelper newNamespace: #NsTestA imports: (Array new: 0).
	NsTestHelper build: 'NsFoo := Object [ tag [ ^#fromA ] ]' in: nsA.

	t check: 'a class built into a namespace lands in its own bindings'
		that: [ (nsA at: #NsFoo) notNil ].
	t check: 'and does NOT leak into core'
		that: [ (Smalltalk includesKey: #NsFoo) not ].
	t check: 'instances of a namespaced class work normally'
		equals: #fromA is: [ (nsA at: #NsFoo) new tag ].
	t check: 'the class records its home namespace'
		that: [ (nsA at: #NsFoo) namespace == nsA ].

	NsTestHelper build: 'NsUser := Object [ use [ ^NsFoo new tag ] core [ ^OrderedCollection new ] ]' in: nsA.
	t check: 'method bodies resolve sibling classes in the same namespace'
		equals: #fromA is: [ (nsA at: #NsUser) new use ].
	t check: 'method bodies still reach core classes through the chain'
		that: [ (nsA at: #NsUser) new core isKindOf: OrderedCollection ].

	nsB := NsTestHelper newNamespace: #NsTestB imports: (Array with: nsA).
	NsTestHelper build: 'NsBridge := Object [ cross [ ^NsFoo new tag ] ]' in: nsB.
	t check: 'an importing namespace resolves the exporter classes'
		equals: #fromA is: [ (nsB at: #NsBridge) new cross ].

	NsTestHelper build: 'NsFoo := Object [ tag [ ^#fromB ] ]' in: nsB.
	t check: 'same class name in another namespace is an independent class'
		that: [ (nsA at: #NsFoo) ~~ (nsB at: #NsFoo) ].
	t check: 'the exporter keeps its own class untouched'
		equals: #fromA is: [ (nsA at: #NsFoo) new tag ].
	NsTestHelper build: 'NsBridge2 := Object [ cross [ ^NsFoo new tag ] ]' in: nsB.
	t check: 'new compiles in B bind B own NsFoo over the import'
		equals: #fromB is: [ (nsB at: #NsBridge2) new cross ].
	t check: 'already-compiled methods keep their compile-time binding'
		equals: #fromA is: [ (nsB at: #NsBridge) new cross ].

	coreOrdColl := Smalltalk at: #OrderedCollection.
	NsTestHelper build: 'OrderedCollection := Object [ marker [ ^#shadow ] ]' in: nsA.
	NsTestHelper build: 'NsShadowUser := Object [ probe [ ^OrderedCollection new marker ] ]' in: nsA.
	t check: 'a package can shadow a core name for its own compiles'
		equals: #shadow is: [ (nsA at: #NsShadowUser) new probe ].
	t check: 'core itself is untouched by the shadow'
		that: [ (Smalltalk at: #OrderedCollection) == coreOrdColl ].

	NsTestHelper build: 'NsFwd := Object [ make [ ^NsLater new ] ]' in: nsA.
	t check: 'a forward reference vivifies in the ACTIVE namespace'
		that: [ nsA includesKey: #NsLater ].
	t check: 'and never lands in core'
		that: [ (Smalltalk includesKey: #NsLater) not ].
	t check: 'unresolvedNames reports the pending forward reference'
		that: [ nsA unresolvedNames includes: #NsLater ].
	NsTestHelper build: 'NsLater := Object [ ]' in: nsA.
	t check: 'defining the class later fills the binding in place'
		that: [ (nsA at: #NsFwd) new make isKindOf: (nsA at: #NsLater) ].
	t check: 'after the definition the name is resolved'
		that: [ (nsA unresolvedNames includes: #NsLater) not ].

	Namespace default: nsA.
	Compiler new evaluate: 'NsEvalGlobal := 42'.
	Namespace default: (Namespaces at: #Core).
	t check: 'eval under a non-core default vivifies globals there'
		equals: 42 is: [ nsA at: #NsEvalGlobal ].
	t check: 'and not in core'
		that: [ (Smalltalk includesKey: #NsEvalGlobal) not ].

	^t report
]
