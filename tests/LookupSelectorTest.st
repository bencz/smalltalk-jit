"Behavior>>lookupSelector: backed by a C primitive.

 The method used to be a superclass-chain walk in Smalltalk, with a dictionary
 send and a block activation per level. It is now BehaviorLookupSelectorPrimitive
 with that same walk kept underneath as the fallback, so the two must agree
 everywhere, including on the answers that are easy to get wrong when a lookup
 moves into C:

  * NOT FOUND is nil, not an error and not a DNU trampoline. The C walk returns
    a null pointer when the chain is exhausted and that has to arrive as nil.
  * INHERITED methods come from the defining class, several levels up.
  * A METACLASS receiver (`Foo class`) walks the metaclass chain, which is a
    different chain from the instance side and terminates differently.
  * An OVERRIDE must answer the subclass's method, not the superclass's: this
    is the one that silently returns a plausible wrong object if the walk
    starts at the wrong end.
  * A selector that is neither Symbol nor String must FAIL the primitive into
    the Smalltalk body rather than dereferencing it as a string.

 The identity assertions matter more than the nil ones: `compiledMethodAt:`
 reads the method dictionary directly with no chain walk, so it is an
 independent oracle for what the walk should have found."

LookupBase := Object [
	greet [ ^#base ]
	shared [ ^#fromBase ]
	class make [ ^self new ]
]

LookupMiddle := LookupBase [
	middleOnly [ ^#middle ]
]

LookupLeaf := LookupMiddle [
	shared [ ^#fromLeaf ]
]

[ | t base leaf found |
	t := TestRun named: 'LookupSelector'.

	"--- found in the receiver's own dictionary ---"
	t check: 'a selector defined on the class itself is found'
		equals: (LookupBase compiledMethodAt: #greet)
		is: [ LookupBase lookupSelector: #greet ].

	"--- inherited, two levels up ---"
	t check: 'an inherited selector answers the DEFINING class method'
		equals: (LookupBase compiledMethodAt: #greet)
		is: [ LookupLeaf lookupSelector: #greet ].

	t check: 'an inherited selector one level up is found'
		equals: (LookupMiddle compiledMethodAt: #middleOnly)
		is: [ LookupLeaf lookupSelector: #middleOnly ].

	"--- override: the walk must stop at the SUBCLASS ---"
	t check: 'an override answers the subclass method, not the superclass one'
		equals: (LookupLeaf compiledMethodAt: #shared)
		is: [ LookupLeaf lookupSelector: #shared ].

	t check: 'the superclass still answers its own method for the same selector'
		equals: (LookupBase compiledMethodAt: #shared)
		is: [ LookupBase lookupSelector: #shared ].

	"--- reaching Object through the whole chain ---"
	found := LookupLeaf lookupSelector: #printString.
	t check: 'a selector defined way up on Object is reached'
		equals: true is: [ found notNil ].

	"--- not found is nil, at every depth ---"
	t check: 'a selector nobody implements answers nil'
		equals: true is: [ (LookupLeaf lookupSelector: #noSuchSelectorAnywhere) isNil ].

	t check: 'not found on a root-ish class answers nil too'
		equals: true is: [ (LookupBase lookupSelector: #noSuchSelectorAnywhere) isNil ].

	"--- metaclass receiver: a different chain ---"
	t check: 'a class-side selector is found on the metaclass'
		equals: (LookupBase class compiledMethodAt: #make)
		is: [ LookupBase class lookupSelector: #make ].

	t check: 'a class-side selector is inherited by a subclass metaclass'
		equals: (LookupBase class compiledMethodAt: #make)
		is: [ LookupLeaf class lookupSelector: #make ].

	t check: 'an instance-side selector is NOT found on the metaclass'
		equals: true is: [ (LookupBase class lookupSelector: #greet) isNil ].

	t check: 'a class-side selector is NOT found on the instance side'
		equals: true is: [ (LookupBase lookupSelector: #make) isNil ].

	"--- non-Symbol selector: the primitive must fail into the .st walk ---"
	t check: 'a String selector still resolves'
		equals: (LookupBase compiledMethodAt: #greet)
		is: [ LookupBase lookupSelector: 'greet' asSymbol ].

	t check: 'a SmallInteger selector answers nil instead of crashing'
		equals: true is: [ (LookupBase lookupSelector: 42) isNil ].

	t check: 'a nil selector answers nil instead of crashing'
		equals: true is: [ (LookupBase lookupSelector: nil) isNil ].

	"--- perform: rides on this walk, so pin it end to end ---"
	base := LookupBase new.
	leaf := LookupLeaf new.
	t check: 'perform: dispatches through the walk'
		equals: #base is: [ base perform: #greet ].
	t check: 'perform: respects an override'
		equals: #fromLeaf is: [ leaf perform: #shared ].
	t check: 'perform:with: still works'
		equals: 7 is: [ 3 perform: #+ with: 4 ].

	"--- hot: the same answers once the call site is compiled and tiered ---"
	found := 0.
	1 to: 5000 do: [:i |
		(LookupLeaf lookupSelector: #shared) == (LookupLeaf compiledMethodAt: #shared)
			ifFalse: [ found := found + 1 ].
		(LookupLeaf lookupSelector: #noSuchSelectorAnywhere) isNil
			ifFalse: [ found := found + 1 ] ].
	t check: 'the walk answers identically when hot'
		equals: 0 is: [ found ].

	^t report
]
