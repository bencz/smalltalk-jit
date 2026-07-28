"Additive class extension: `Name extend [ methods ]` adds methods to an
 EXISTING class without touching its shape or its other methods. Covers the
 top-level parser path (this file itself extends String), the metaclass
 side, atomicity on collision with an own selector, namespace semantics
 (extension method globals resolve in the EXTENDING namespace), inherited
 override locality, and removeSelector:."

String extend [

	extMarker [
		^'ext:', self
	]

	class extClassMarker [
		^#classSide
	]

]

ExtParent := Object [
	probe [ ^#parent ]
]

ExtChild := ExtParent [
]

[ | t ns before survivors missing |
	t := TestRun named: 'ClassExtension'.

	t check: 'a top-level extension adds an instance method to a core class'
		equals: 'ext:abc' is: [ 'abc' extMarker ].
	t check: 'existing instances see the extension (method dict lookup, no shape change)'
		equals: 'ext:xy' is: [ ('x', 'y') extMarker ].
	t check: 'the metaclass side extends through `class selector`'
		equals: #classSide is: [ String extClassMarker ].
	t check: 'the extended class keeps its identity and its old methods'
		equals: 3 is: [ 'abc' size ].

	"atomicity: one collision with an OWN selector (asSymbol lives in String's
	 own dictionary; an INHERITED selector like size would be a legal
	 override) aborts the WHOLE extension"
	t expect: 'extending with an existing own selector raises RedefinitionError'
		raises: RedefinitionError in: [
			Compiler new buildClass:
				(Parser parseString: 'String extend [ asSymbol [ ^0 ] extNeverLands [ ^1 ] ]') parseClass ].
	t check: 'and nothing from the failed extension installed'
		that: [ ('abc' respondsTo: #extNeverLands) not ].
	t check: 'asSymbol still answers the original method'
		that: [ 'abc' asSymbol == #abc ].

	"extension from a namespace: bodies resolve in the EXTENDING namespace"
	ns := Namespace named: #ExtNsProbe imports: (Array new: 0).
	Namespaces at: #ExtNsProbe put: ns.
	Compiler new buildClass:
		(Parser parseString: 'ExtNsHelper := Object [ class tag [ ^#fromExtNs ] ]') parseClass
		in: ns.
	Compiler new buildClass:
		(Parser parseString: 'Object extend [ extNsProbe [ ^ExtNsHelper tag ] ]') parseClass
		in: ns.
	t check: 'extension methods resolve globals in the extending namespace'
		equals: #fromExtNs is: [ 42 extNsProbe ].
	t check: 'the extended class home namespace is unchanged'
		that: [ (Smalltalk at: #Object) namespace == (Namespaces at: #Core) ].

	"extending an unknown class is an error"
	t expect: 'extending an undefined class raises' raises: Error in: [
		Compiler new buildClass:
			(Parser parseString: 'NoSuchClassZzz extend [ foo [ ^1 ] ]') parseClass ].

	"inherited override through extend: only the extended class changes"
	before := ExtParent new probe.
	Compiler new buildClass:
		(Parser parseString: 'ExtChild extend [ probe [ ^#child ] ]') parseClass.
	t check: 'the parent still answers its own method' equals: #parent is: [ ExtParent new probe ].
	t check: 'the extended subclass now overrides' equals: #child is: [ ExtChild new probe ].
	t check: 'the parent answered the same before and after' equals: #parent is: [ before ].

	"removeSelector: undoes an extension"
	Object removeSelector: #extNsProbe.
	t check: 'removeSelector: removed the method'
		that: [ (42 respondsTo: #extNsProbe) not ].
	t expect: 'removing a selector that is not defined here raises'
		raises: NotFoundError in: [ Object removeSelector: #extNsProbe ].
	t expect: 'removing an inherited selector from a subclass raises (own dict only)'
		raises: NotFoundError in: [ ExtChild removeSelector: #printString ].
	"An IMMEDIATE selector used to reach asObject inside the primitive, which
	 aborts the VM on a debug build and dereferences a wild pointer on a release
	 one. It has to fail the primitive into the Smalltalk body and raise."
	t expect: 'removing with a SmallInteger selector raises instead of crashing'
		raises: NotFoundError in: [ ExtChild removeSelector: 42 ].
	t expect: 'removing with a nil selector raises instead of crashing'
		raises: NotFoundError in: [ ExtChild removeSelector: nil ].

	"THE PROBE CHAIN SURVIVES A REMOVAL, which is the silent half of removing
	 from an open-addressed dictionary. Probing is linear and stops at the first
	 empty slot, so emptying a slot cuts every chain that ran through it: an
	 unrelated selector whose home index is before the hole and whose entry is
	 after it becomes unfindable while still occupying a slot.

	 So this removes ONE selector and then looks for the ones it did NOT remove.
	 Enough of them that collisions are certain, and every one is checked,
	 because a cut chain loses an arbitrary subset."
	Compiler new buildClass: (Parser parseString:
		'ExtManySelectors := Object [
			s01 [ ^1 ] s02 [ ^2 ] s03 [ ^3 ] s04 [ ^4 ] s05 [ ^5 ] s06 [ ^6 ]
			s07 [ ^7 ] s08 [ ^8 ] s09 [ ^9 ] s10 [ ^10 ] s11 [ ^11 ] s12 [ ^12 ]
			s13 [ ^13 ] s14 [ ^14 ] s15 [ ^15 ] s16 [ ^16 ] s17 [ ^17 ] s18 [ ^18 ]
			s19 [ ^19 ] s20 [ ^20 ] s21 [ ^21 ] s22 [ ^22 ] s23 [ ^23 ] s24 [ ^24 ]
		]') parseClass.
	survivors := OrderedCollection new.
	1 to: 24 do: [ :i |
		survivors add: (i < 10
			ifTrue: [ ('s0', i printString) asSymbol ]
			ifFalse: [ ('s', i printString) asSymbol ]) ].
	"remove a third of them, spread through the table"
	1 to: 24 by: 3 do: [ :i | ExtManySelectors removeSelector: (survivors at: i) ].
	missing := OrderedCollection new.
	1 to: 24 do: [ :i |
		| selector present |
		selector := survivors at: i.
		present := ExtManySelectors new respondsTo: selector.
		(i - 1) \\ 3 = 0
			ifTrue: [ present ifTrue: [ missing add: selector -> #stillThere ] ]
			ifFalse: [ present ifFalse: [ missing add: selector -> #lost ] ] ].
	t check: 'removing selectors leaves every other one findable'
		equals: 0 is: [ missing size ].
	t check: 'and the survivors still answer their own value'
		equals: 24 is: [ ExtManySelectors new s24 ].

	^t report
]
