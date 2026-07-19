"Source-level namespace declarations: `Name := Namespace [ classDefs ]`
 declares (or reopens) a namespace without any package involved. Covers the
 top-level parser path (this file declares two below), binding of the
 namespace object in the declaring namespace, member independence across
 namespaces, reopening, member class-side initialize, extensions inside a
 body, member forward references, and the declaring-namespace import."

DeclBanner := Namespace [

	DeclFormatter := Object [
		class format: aString [ ^'== ', aString, ' ==' ]
	]

	DeclUser := Object [
		use [ ^DeclFormatter format: 'sibling' ]
		late [ ^DeclLate tag ]
	]

	DeclLate := Object [
		class tag [ ^#late ]
	]

	DeclCounted := Object [
		| Count |
		class initialize [ Count := 41 ]
		class bump [ Count := Count + 1. ^Count ]
	]

]

DeclPlain := Namespace [

	DeclFormatter := Object [
		class format: aString [ ^aString ]
	]

	String extend [ declShout [ ^self asUppercase, '?' ] ]

]

[ | t outerNs innerNs |
	t := TestRun named: 'NamespaceDecl'.

	t check: 'a declaration registers the namespace'
		that: [ (Namespaces at: #DeclBanner) notNil ].
	t check: 'the namespace object is bound in the declaring namespace'
		that: [ DeclBanner == (Namespaces at: #DeclBanner) ].
	t check: 'members live in the declared namespace, not the declaring one'
		that: [ (Smalltalk includesKey: #DeclFormatter) not ].
	t check: 'members are reachable through the namespace object'
		equals: '== x ==' is: [ (DeclBanner at: #DeclFormatter) format: 'x' ].
	t check: 'same member name in two declared namespaces is independent'
		equals: 'x' is: [ (DeclPlain at: #DeclFormatter) format: 'x' ].
	t check: 'members resolve their siblings unqualified'
		equals: '== sibling ==' is: [ (DeclBanner at: #DeclUser) new use ].
	t check: 'forward references between members work'
		equals: #late is: [ (DeclBanner at: #DeclUser) new late ].
	t check: 'member class-side initialize ran at declaration end'
		equals: 42 is: [ (DeclBanner at: #DeclCounted) bump ].
	t check: 'an extension inside a namespace body reaches the core class'
		equals: 'HI?' is: [ 'hi' declShout ].
	t check: 'qualified names name the declared namespace'
		equals: 'DeclBanner.DeclFormatter' is: [ (DeclBanner at: #DeclFormatter) qualifiedName ].

	"reopening adds members and keeps the old ones"
	Compiler new buildClass: (Parser parseString:
		'DeclBanner := Namespace [ DeclMore := Object [ class tag [ ^#more ] ] ]') parseClass.
	t check: 'reopening a namespace adds new members'
		equals: #more is: [ (DeclBanner at: #DeclMore) tag ].
	t check: 'and keeps the existing ones'
		equals: '== x ==' is: [ (DeclBanner at: #DeclFormatter) format: 'x' ].

	"a namespace declared inside a NON-core namespace imports the declarant:
	 its members see the declarant's classes unqualified"
	outerNs := Namespace named: #DeclOuter imports: (Array new: 0).
	Namespaces at: #DeclOuter put: outerNs.
	Compiler new buildClass: (Parser parseString:
		'DeclHelper := Object [ class tag [ ^#outerHelper ] ]') parseClass in: outerNs.
	Compiler new buildClass: (Parser parseString:
		'DeclInner := Namespace [ DeclInnerUser := Object [ probe [ ^DeclHelper tag ] ] ]') parseClass
		in: outerNs.
	innerNs := Namespaces at: #DeclInner.
	t check: 'a namespace declared in a package namespace imports it'
		equals: #outerHelper is: [ (innerNs at: #DeclInnerUser) new probe ].
	t check: 'and the namespace object is bound in the declarant, not core'
		that: [ (outerNs includesKey: #DeclInner) and: [ (Smalltalk includesKey: #DeclInner) not ] ].

	^t report
]
