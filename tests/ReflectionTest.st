"Small hierarchy used to exercise Object reflection protocol. RfltB inherits
 foo from RfltA and adds bar, so we can check inherited vs local behaviour."
RfltA := Object [
    | av |
    foo [ ^1 ]
    twice: x [ ^x + x ]
    plus: x with: y [ ^x + y ]
    setAv: x [ av := x ]
    av [ ^av ]
]
RfltB := RfltA [ bar [ ^2 ] ]

[
    | t a b arr assoc s1 s2 |
    t := TestRun named: 'Reflection'.

    "--- class ---"
    t check: 'Object new class == Object' that: [ Object new class == Object ].
    t check: 'literal SmallInteger class' that: [ 3 class == SmallInteger ].
    t check: 'user instance class' that: [ RfltA new class == RfltA ].
    t check: 'nil class is UndefinedObject' that: [ nil class == UndefinedObject ].
    t check: 'true class is True' that: [ true class == True ].
    t check: 'species answers class' that: [ Object new species == Object ].

    "--- isKindOf: / isMemberOf: ---"
    t check: 'isKindOf self class' that: [ RfltB new isKindOf: RfltB ].
    t check: 'isKindOf superclass' that: [ RfltB new isKindOf: RfltA ].
    t check: 'isKindOf Object root' that: [ RfltB new isKindOf: Object ].
    t check: 'not isKindOf subclass' that: [ (RfltA new isKindOf: RfltB) = false ].
    t check: 'integer isKindOf Object' that: [ 1 isKindOf: Object ].
    t check: 'integer not isKindOf UndefinedObject' that: [ (1 isKindOf: UndefinedObject) = false ].
    t check: 'isMemberOf exact class' that: [ RfltB new isMemberOf: RfltB ].
    t check: 'not isMemberOf superclass' that: [ (RfltB new isMemberOf: RfltA) = false ].

    "--- respondsTo: ---"
    t check: 'respondsTo inherited selector' that: [ RfltB new respondsTo: #foo ].
    t check: 'respondsTo local selector' that: [ RfltB new respondsTo: #bar ].
    t check: 'superclass does not respondTo subclass selector' that: [ (RfltA new respondsTo: #bar) = false ].
    t check: 'respondsTo missing is false' that: [ (RfltA new respondsTo: #noSuchThing) = false ].
    t check: 'integer respondsTo +' that: [ 3 respondsTo: #+ ].

    "--- perform: family (must equal the direct send) ---"
    t check: 'perform: unary' equals: 1 is: [ RfltA new perform: #foo ].
    t check: 'perform:with:' equals: 84 is: [ RfltA new perform: #twice: with: 42 ].
    t check: 'perform:with:with:' equals: 7 is: [ RfltA new perform: #plus:with: with: 3 with: 4 ].
    t check: 'perform: + equals direct' equals: 7 is: [ 3 perform: #+ with: 4 ].
    t check: 'perform:with:with: on integer' equals: true is: [ 1 perform: #between:and: with: 0 with: 2 ].
    t check: 'perform:withArguments:' equals: true is: [ 3 perform: #between:and: withArguments: (Array with: 1 with: 5) ].
    t check: 'perform:withArguments: empty' equals: 1 is: [ RfltA new perform: #foo withArguments: #() ].

    "--- identity vs equality ---"
    a := Object new.
    b := Object new.
    t check: 'identical == self' that: [ a == a ].
    t check: 'distinct not ==' that: [ (a == b) = false ].
    t check: '~~ distinct is true' that: [ a ~~ b ].
    t check: '~~ self is false' that: [ (a ~~ a) = false ].
    t check: '= self' that: [ a = a ].
    t check: 'distinct not =' that: [ (a = b) = false ].
    t check: '~= distinct is true' that: [ a ~= b ].
    t check: '~= self is false' that: [ (a ~= a) = false ].

    "value equality: equal-but-not-identical strings"
    s1 := 'hello' copy.
    s2 := 'hello' copy.
    t check: 'equal strings =' that: [ s1 = s2 ].
    t check: 'equal strings not identical' that: [ (s1 == s2) = false ].
    t check: 'equal strings ~~' that: [ s1 ~~ s2 ].

    "--- hash / identityHash ---"
    t check: 'hash is SmallInteger' that: [ a hash isKindOf: SmallInteger ].
    t check: 'hash stable on same object' that: [ a hash = a hash ].
    t check: 'identityHash stable on same object' that: [ a identityHash == a identityHash ].
    t check: 'content hash equal for equal strings' that: [ s1 hash = s2 hash ].

    "--- copy / shallowCopy / yourself ---"
    t check: 'copy is not identical' that: [ (a == a copy) = false ].
    t check: 'shallowCopy is not identical' that: [ (a == a shallowCopy) = false ].
    t check: 'copy has same class' that: [ a copy class == a class ].
    arr := Array with: a with: b.
    t check: 'shallowCopy shares elements' that: [ (arr shallowCopy at: 1) == a ].
    t check: 'shallowCopy equal to original array' that: [ arr shallowCopy = arr ].
    t check: 'deepCopy does not share elements' that: [ ((arr deepCopy at: 1) == a) = false ].
    t check: 'yourself returns self' that: [ a yourself == a ].

    "--- -> association ---"
    assoc := 3 -> 4.
    t check: 'assoc is an Association' that: [ assoc class == Association ].
    t check: 'assoc key' equals: 3 is: [ assoc key ].
    t check: 'assoc value' equals: 4 is: [ assoc value ].

    "--- isNil / notNil ---"
    t check: 'nil isNil' that: [ nil isNil ].
    t check: 'nil notNil is false' that: [ nil notNil = false ].
    t check: 'object isNil is false' that: [ a isNil = false ].
    t check: 'object notNil' that: [ a notNil ].

    "--- error path: perform: of a missing selector raises MessageNotUnderstood ---"
    "(perform: looks the selector up, gets nil, then sends sendTo: to nil which
      is not understood). Bound to a temp receiver to avoid the literal-DNU bug."
    t expect: 'perform: missing raises MNU' raises: MessageNotUnderstood in: [
        | r | r := RfltA new. r perform: #definitelyNotASelector ].

    ^t report
]
