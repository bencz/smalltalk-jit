[
    | t |
    t := TestRun named: 'MiscRuntime'.

    "=== SourceCode ===
     Plain value object: source/position/sourceSize/line/column accessors plus
     a printOn: that renders source:line:column. Nothing constructs it from a
     string in the kernel, so a fresh instance has all-nil fields; the fields
     are populated reflectively here (instVarAt:put:) to exercise the accessors
     and printOn: without touching the filesystem."
    t check: 'SourceCode loaded' that: [ SourceCode isNil not ].
    t check: 'SourceCode name' that: [ SourceCode name = #SourceCode ].
    t check: 'SourceCode superClass is Object' that: [ SourceCode superClass == Object ].
    t check: 'SourceCode has 5 instance vars' equals: 5 is: [ SourceCode instanceVariables size ].
    t check: 'SourceCode ivar names' that: [ SourceCode instanceVariables = #('source' 'position' 'sourceSize' 'line' 'column') ].
    t check: 'fresh SourceCode source is nil' that: [ SourceCode new source isNil ].
    t check: 'fresh SourceCode position is nil' that: [ SourceCode new position isNil ].
    t check: 'fresh SourceCode sourceSize is nil' that: [ SourceCode new sourceSize isNil ].
    t check: 'fresh SourceCode line is nil' that: [ SourceCode new line isNil ].
    t check: 'fresh SourceCode column is nil' that: [ SourceCode new column isNil ].
    t check: 'fresh SourceCode sourceContents mirrors source (nil)' that: [ SourceCode new sourceContents isNil ].
    t check: 'SourceCode accessors return set fields' that: [
        | sc |
        sc := SourceCode new.
        sc instVarAt: 1 put: 'abc'.
        sc instVarAt: 4 put: 2.
        sc instVarAt: 5 put: 5.
        (sc source = 'abc')
            and: [ sc line = 2
            and: [ sc column = 5
            and: [ sc sourceContents = 'abc' ] ] ] ].
    t check: 'SourceCode printOn: renders source:line:column' equals: '''abc'':2:5'
        is: [
            | sc |
            sc := SourceCode new.
            sc instVarAt: 1 put: 'abc'.
            sc instVarAt: 4 put: 2.
            sc instVarAt: 5 put: 5.
            sc printString ].

    "=== FileSourceCode ===
     Subclass of SourceCode whose sourceContents reads from a FileStream. That
     path needs a real file on disk, so it is not exercised here; only the
     inheritance wiring and the inherited (non-file) accessors are checked."
    t check: 'FileSourceCode loaded' that: [ FileSourceCode isNil not ].
    t check: 'FileSourceCode name' that: [ FileSourceCode name = #FileSourceCode ].
    t check: 'FileSourceCode superClass is SourceCode' that: [ FileSourceCode superClass == SourceCode ].
    t check: 'FileSourceCode inheritsFrom SourceCode' that: [ FileSourceCode inheritsFrom: SourceCode ].
    t check: 'FileSourceCode inherits source accessor' that: [ FileSourceCode new source isNil ].
    t check: 'FileSourceCode inherits position accessor' that: [ FileSourceCode new position isNil ].

    "=== Message ===
     selector/arguments value object. Also produced by the VM itself when a
     receiver does not understand a selector: doesNotUnderstand: hands the
     handler a real Message. The receiver is bound to a temp (never a bare
     literal) to steer clear of the known literal-DNU crash."
    t check: 'Message loaded' that: [ Message isNil not ].
    t check: 'Message name' that: [ Message name = #Message ].
    t check: 'Message superClass is Object' that: [ Message superClass == Object ].
    t check: 'Message has 2 instance vars' equals: 2 is: [ Message instanceVariables size ].
    t check: 'Message ivar names' that: [ Message instanceVariables = #('selector' 'arguments') ].
    t check: 'fresh Message selector is nil' that: [ Message new selector isNil ].
    t check: 'fresh Message arguments is nil' that: [ Message new arguments isNil ].
    t check: 'Message accessors return set fields' that: [
        | m |
        m := Message new.
        m instVarAt: 1 put: #foo:bar:.
        m instVarAt: 2 put: #(1 2).
        (m selector == #foo:bar:) and: [ m arguments = #(1 2) ] ].

    t check: 'DNU builds Message with unary selector' that: [
        | r sel |
        r := Object new.
        [ r zorkifyPlease ] on: MessageNotUnderstood do: [ :e | sel := e message selector ].
        sel == #zorkifyPlease ].
    t check: 'DNU unary Message has empty argument Array' that: [
        | r args |
        r := Object new.
        [ r zorkifyPlease ] on: MessageNotUnderstood do: [ :e | args := e message arguments ].
        (args isKindOf: Array) and: [ args size = 0 ] ].
    t check: 'DNU builds Message with keyword selector' that: [
        | r sel |
        r := Object new.
        [ r zork: 1 plus: 2 ] on: MessageNotUnderstood do: [ :e | sel := e message selector ].
        sel == #zork:plus: ].
    t check: 'DNU keyword Message argument Array has arity 2' that: [
        | r args |
        r := Object new.
        [ r zork: 1 plus: 2 ] on: MessageNotUnderstood do: [ :e | args := e message arguments ].
        (args isKindOf: Array) and: [ args size = 2 ] ].
    t check: 'DNU error carries the offending receiver' that: [
        | r rr |
        r := Object new.
        [ r zorkifyPlease ] on: MessageNotUnderstood do: [ :e | rr := e receiver ].
        rr == r ].
    t check: 'DNU error message is a Message' that: [
        | r m |
        r := Object new.
        [ r zorkifyPlease ] on: MessageNotUnderstood do: [ :e | m := e message ].
        m isKindOf: Message ].

    "=== Cross-cutting: system globals and class reflection ===
     The Smalltalk global is the symbol-keyed dictionary of globals; class-name
     lookup goes through it. SymbolTable is the interned-symbol Array."
    t check: 'Smalltalk is a Dictionary' that: [ Smalltalk isKindOf: Dictionary ].
    t check: 'Smalltalk resolves a class by name' that: [ (Smalltalk at: #Object) == Object ].
    t check: 'Smalltalk resolves SmallInteger by name' that: [ (Smalltalk at: #SmallInteger) == SmallInteger ].
    t check: 'Smalltalk includesKey: known global' that: [ Smalltalk includesKey: #Message ].
    t check: 'Smalltalk at:ifAbsent: runs block for unknown name' equals: #missing
        is: [ Smalltalk at: #NoSuchGlobalXyzzy ifAbsent: [ #missing ] ].
    t check: 'SymbolTable is an Array' that: [ SymbolTable isKindOf: Array ].
    t check: 'SymbolTable is non-empty' that: [ SymbolTable size > 0 ].

    "Class>>name yields a Symbol; Class>>printOn: renders that name; the
     metaclass prints as '<name> class'."
    t check: 'class name is a Symbol' that: [ Object name isKindOf: Symbol ].
    t check: 'Object name' that: [ Object name = #Object ].
    t check: 'Object printString is its name' equals: 'Object' is: [ Object printString ].
    t check: 'SmallInteger printString is its name' equals: 'SmallInteger' is: [ SmallInteger printString ].
    t check: 'metaclass printString appends class' equals: 'Object class' is: [ Object class printString ].
    t check: 'metaclass name mirrors instance class name' that: [ Object class name = #Object ].

    "Object>>-> builds an Association (cross-cutting associating protocol)."
    t check: 'Object>>-> key' equals: 1 is: [ (1 -> 2) key ].
    t check: 'Object>>-> value' equals: 2 is: [ (1 -> 2) value ].

    ^t report
]
