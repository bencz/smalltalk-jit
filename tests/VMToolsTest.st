"A concrete Iterator subclass: the base class leaves do:/select:/collect: as
 SubClassResponsibility, so the reusable protocol (inject:into:, detect:,
 detect:ifNone:, anySatisfy:) can only be exercised through a subclass that
 supplies do:. This one iterates a fixed backing Array."
VMToolsTestIter := Iterator [
    | items |
    items: anArray [ items := anArray. ^self ]
    do: aBlock [ items do: [ :e | aBlock value: e ] ]
]

[
    | t |
    t := TestRun named: 'VMTools'.

    "=== VMTools ===
     WARNING: in this image the VMTools global is bound to nil, not to a class.
     Its only methods (printCCharacters/printCCharacterClasses/
     printCCharacterDigitValues) are Transcript dump generators with no
     value-returning surface, and the class itself is not present in the built
     image. The one deterministic, safe observation is that the global name is
     registered but resolves to nil."
    t check: 'VMTools global is nil (not loaded as a class)' that: [ VMTools isNil ].
    t check: 'VMTools symbol registered in Smalltalk globals' that: [ Smalltalk includesKey: #VMTools ].

    "=== TypeFeedback ===
     Internal JIT bookkeeping object: two instance variables (ic, hintedClass)
     and no behaviour methods. Only its structural shape is safely testable."
    t check: 'TypeFeedback loaded' that: [ TypeFeedback isNil not ].
    t check: 'TypeFeedback name' that: [ TypeFeedback name = #TypeFeedback ].
    t check: 'TypeFeedback superClass is Object' that: [ TypeFeedback superClass == Object ].
    t check: 'TypeFeedback has 2 instance vars' equals: 2 is: [ TypeFeedback instanceVariables size ].
    t check: 'TypeFeedback ivar names' that: [ TypeFeedback instanceVariables = #('ic' 'hintedClass') ].
    t check: 'TypeFeedback new is a TypeFeedback' that: [ TypeFeedback new class == TypeFeedback ].
    t check: 'TypeFeedback new ic starts nil' that: [ (TypeFeedback new instVarAt: 1) isNil ].
    t check: 'TypeFeedback new hintedClass starts nil' that: [ (TypeFeedback new instVarAt: 2) isNil ].

    "=== Iterator ===
     Abstract enumeration mixin. do:/select:/collect: are subclass
     responsibility; inject:into:/detect:/detect:ifNone:/anySatisfy: are
     concrete and defined purely in terms of do:."
    t expect: 'Iterator>>do: is subclassResponsibility'
        raises: SubClassResponsibility in: [ Iterator new do: [ :x | x ] ].
    t expect: 'Iterator>>select: is subclassResponsibility'
        raises: SubClassResponsibility in: [ Iterator new select: [ :x | x ] ].
    t expect: 'Iterator>>collect: is subclassResponsibility'
        raises: SubClassResponsibility in: [ Iterator new collect: [ :x | x ] ].

    t check: 'inject:into: folds via do:' equals: 15
        is: [ (VMToolsTestIter new items: #(1 2 3 4 5)) inject: 0 into: [ :a :b | a + b ] ].
    t check: 'inject:into: on empty is seed' equals: 42
        is: [ (VMToolsTestIter new items: #()) inject: 42 into: [ :a :b | a + b ] ].
    t check: 'detect: finds first match' equals: 4
        is: [ (VMToolsTestIter new items: #(1 2 3 4 5)) detect: [ :x | x > 3 ] ].
    t check: 'detect:ifNone: returns hit' equals: 2
        is: [ (VMToolsTestIter new items: #(1 2 3 4 5)) detect: [ :x | x = 2 ] ifNone: [ 0 ] ].
    t check: 'detect:ifNone: runs none block' equals: 99
        is: [ (VMToolsTestIter new items: #(1 2 3 4 5)) detect: [ :x | x > 9 ] ifNone: [ 99 ] ].
    t expect: 'detect: with no match raises NotFoundError'
        raises: NotFoundError in: [ (VMToolsTestIter new items: #(1 2 3 4 5)) detect: [ :x | x > 9 ] ].
    t check: 'anySatisfy: true when one matches' that: [ (VMToolsTestIter new items: #(1 2 3 4 5)) anySatisfy: [ :x | x > 4 ] ].
    t check: 'anySatisfy: false when none match' that: [ ((VMToolsTestIter new items: #(1 2 3 4 5)) anySatisfy: [ :x | x > 9 ]) = false ].
    t check: 'anySatisfy: false on empty' that: [ ((VMToolsTestIter new items: #()) anySatisfy: [ :x | true ]) = false ].

    "=== GarbageCollector ===
     Requesting a collection must leave the system running and previously
     allocated live objects intact and equal. Allocate a small mixed graph,
     force two collections, and re-verify every field. Bounded allocation, no
     stress loops (see GcStressTest / LoopCarriedRootTest for the heavy cases)."
    t check: 'GarbageCollector loaded' that: [ GarbageCollector isNil not ].
    t check: 'GarbageCollector name' that: [ GarbageCollector name = #GarbageCollector ].
    t check: 'collectGarbage returns non-nil (survives)' that: [ GarbageCollector collectGarbage notNil ].

    t check: 'live objects survive a forced collection' that: [
        | arr oc nested sum |
        arr := Array with: 11 with: 22 with: 33.
        oc := OrderedCollection new.
        1 to: 20 do: [ :i | oc add: i * 3 ].
        nested := Array with: (Array with: 'x' with: 'y') with: (7 -> 'seven').
        GarbageCollector collectGarbage.
        GarbageCollector collectGarbage.
        sum := 0.
        oc do: [ :v | sum := sum + v ].
        (arr = (Array with: 11 with: 22 with: 33))
            and: [ (arr at: 2) = 22
            and: [ oc size = 20
            and: [ (oc at: 1) = 3
            and: [ (oc at: 20) = 60
            and: [ sum = 630
            and: [ (nested at: 1) = (Array with: 'x' with: 'y')
            and: [ (nested at: 2) key = 7
            and: [ (nested at: 2) value = 'seven' ] ] ] ] ] ] ] ] ].

    t check: 'identity preserved across collection' that: [
        | obj h |
        obj := Object new.
        h := obj identityHash.
        GarbageCollector collectGarbage.
        obj identityHash = h ].

    "lastStats reports a String-keyed Dictionary of non-negative counters."
    t check: 'lastStats is a Dictionary' that: [ GarbageCollector lastStats isKindOf: Dictionary ].
    t check: 'lastStats has count key' that: [ GarbageCollector lastStats includesKey: 'count' ].
    t check: 'lastStats count is non-negative' that: [ (GarbageCollector lastStats at: 'count') >= 0 ].
    t check: 'lastStats total is non-negative' that: [ (GarbageCollector lastStats at: 'total') >= 0 ].
    t check: 'lastStats freed is non-negative' that: [ (GarbageCollector lastStats at: 'freed') >= 0 ].
    t check: 'lastStats missing key runs ifAbsent block' equals: -1
        is: [ GarbageCollector lastStats at: 'noSuchKey' ifAbsent: [ -1 ] ].

    ^t report
]
