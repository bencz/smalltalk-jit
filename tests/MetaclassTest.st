"Three-level hierarchy for metaobject / Behavior protocol. Each level adds one
 local selector so we can separate local selectors from inherited ones."
MetaA := Object [
    | mv |
    foo [ ^1 ]
    setMv: x [ mv := x ]
    mv [ ^mv ]
]
MetaB := MetaA [ bar [ ^2 ] ]
MetaC := MetaB [ baz [ ^3 ] ]

[
    | t seen |
    t := TestRun named: 'Metaclass'.

    "--- name (answers a Symbol) ---"
    t check: 'class name' equals: #MetaA is: [ MetaA name ].
    t check: 'subclass name' equals: #MetaB is: [ MetaB name ].
    t check: 'name is a Symbol' that: [ MetaA name class == Symbol ].
    t check: 'metaclass name mirrors class name' equals: #MetaA is: [ MetaA class name ].

    "--- superclass chain ---"
    t check: 'immediate superclass' that: [ MetaB superClass == MetaA ].
    t check: 'root superclass is Object' that: [ MetaA superClass == Object ].
    t check: 'grandchild superclass' that: [ MetaC superClass == MetaB ].
    t check: 'Object superclass is nil' that: [ Object superClass isNil ].

    "--- inheritsFrom: (strict, walks the chain) ---"
    t check: 'inheritsFrom parent' that: [ MetaB inheritsFrom: MetaA ].
    t check: 'inheritsFrom grandparent' that: [ MetaC inheritsFrom: MetaA ].
    t check: 'inheritsFrom Object' that: [ MetaC inheritsFrom: Object ].
    t check: 'does not inherit from descendant' that: [ (MetaA inheritsFrom: MetaB) = false ].
    t check: 'not inheritsFrom self' that: [ (MetaA inheritsFrom: MetaA) = false ].

    "--- subclasses ---"
    t check: 'subClasses includes child' that: [ MetaA subClasses includes: MetaB ].
    t check: 'subClasses excludes grandchild (direct only)' that: [ (MetaA subClasses includes: MetaC) = false ].
    seen := OrderedCollection new.
    MetaA allSubClassesDo: [:c | seen add: c ].
    t check: 'allSubClassesDo reaches child' that: [ seen includes: MetaB ].
    t check: 'allSubClassesDo reaches grandchild' that: [ seen includes: MetaC ].

    "--- selectors / includesSelector: (local) vs allSelectors / canUnderstand: (inherited) ---"
    t check: 'selectors includes local' that: [ MetaB selectors includes: #bar ].
    t check: 'selectors excludes inherited' that: [ (MetaB selectors includes: #foo) = false ].
    t check: 'includesSelector local' that: [ MetaB includesSelector: #bar ].
    t check: 'includesSelector excludes inherited' that: [ (MetaB includesSelector: #foo) = false ].
    t check: 'allSelectors includes inherited' that: [ MetaC allSelectors includes: #foo ].
    t check: 'allSelectors includes local' that: [ MetaC allSelectors includes: #baz ].
    t check: 'allSelectors includes Object protocol' that: [ MetaC allSelectors includes: #isNil ].
    t check: 'canUnderstand local' that: [ MetaC canUnderstand: #baz ].
    t check: 'canUnderstand inherited foo' that: [ MetaC canUnderstand: #foo ].
    t check: 'canUnderstand Object protocol' that: [ MetaC canUnderstand: #isNil ].
    t check: 'ancestor cannot understand descendant selector' that: [ (MetaA canUnderstand: #bar) = false ].

    "--- lookupSelector: / compiledMethodAt: ---"
    t check: 'lookupSelector finds inherited' that: [ (MetaB lookupSelector: #foo) notNil ].
    t check: 'lookupSelector missing is nil' that: [ (MetaB lookupSelector: #nope) isNil ].
    t check: 'compiledMethodAt local' that: [ (MetaB compiledMethodAt: #bar) notNil ].

    "--- instance variables / shape ---"
    t check: 'instanceVariables is an Array' that: [ MetaA instanceVariables class == Array ].
    t check: 'instanceVariables includes declared ivar' that: [ MetaA instanceVariables includes: 'mv' ].
    t check: 'instanceSize counts ivars' equals: 1 is: [ MetaA instanceSize ].

    "--- new / basicNew ---"
    t check: 'new answers an instance of the class' that: [ MetaA new class == MetaA ].
    t check: 'basicNew answers an instance of the class' that: [ MetaA basicNew class == MetaA ].
    t check: 'new instance understands its method' equals: 1 is: [ MetaA new foo ].

    "--- metaclass relationships ---"
    t check: 'metaclass of a class is a MetaClass' that: [ MetaA class class == MetaClass ].
    t check: 'metaclass isMemberOf MetaClass' that: [ MetaA class isMemberOf: MetaClass ].
    t check: 'metaclass is a ClassDescription' that: [ MetaA class isKindOf: ClassDescription ].
    t check: 'metaclass is a Behavior' that: [ MetaA class isKindOf: Behavior ].
    t check: 'metaclass is not a Class' that: [ (MetaA class isKindOf: Class) = false ].
    t check: 'metaclass instanceClass points back' that: [ MetaA class instanceClass == MetaA ].
    t check: 'a class is an instance of its metaclass' that: [ MetaA isMemberOf: MetaA class ].
    t check: 'a class is a Behavior' that: [ MetaA isKindOf: Behavior ].
    t check: 'a class is a Class' that: [ MetaA isKindOf: Class ].

    "metaclass superclass chain parallels the class chain"
    t check: 'metaclass superclass parallels' that: [ MetaB class superClass == MetaA class ].
    t check: 'root metaclass superclass is Object metaclass' that: [ MetaA class superClass == Object class ].
    t check: 'Object metaclass superclass is Class' that: [ Object class superClass == Class ].
    t check: 'MetaClass metaclass is a MetaClass' that: [ MetaClass class class == MetaClass ].

    ^t report
]
