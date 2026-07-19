"=== SymbolTest: interning identity, comparison, printing, size/at:, immutability,
 and the String protocol Symbol inherits. Symbols are IMMUTABLE (at:put: raises)
 and their string-producing operations answer a plain String (species = String),
 since a Symbol cannot be built by mutation. Goes beyond the existing
 StringTest/CharacterTest/HashPrimitiveTest. ==="
[
    | t |
    t := TestRun named: 'SymbolTest'.

    "--- interning identity ---"
    t check: 'literal interning #foo==#foo' that: [ #foo == #foo ].
    t check: 'String>>asSymbol interns to the literal' that: [ 'foo' asSymbol == #foo ].
    t check: 'concat then asSymbol interns to the same object' that: [ ('fo' , 'o') asSymbol == #foo ].
    t check: 'Symbol>>asSymbol returns self (already interned)' that: [ #hello asSymbol == #hello ].
    t check: 'asSymbol answers a Symbol' that: [ 'foo' asSymbol class == Symbol ].
    t check: 'distinct symbols are not identical' that: [ (#foo == #bar) not ].

    "--- equality ---"
    t check: 'sym = sym (same)' that: [ #foo = #foo ].
    t check: 'sym = sym (diff) is false' that: [ (#foo = #bar) not ].
    t check: 'sym ~= diff sym' that: [ #foo ~= #bar ].
    t check: 'symbol not equal to equal-looking string' that: [ (#hello = 'hello') not ].
    t check: 'string not equal to equal-looking symbol' that: [ ('hello' = #hello) not ].

    "--- printing ---"
    t check: 'unary symbol printString' equals: '#foo' is: [ #foo printString ].
    t check: 'keyword symbol printString' equals: '#at:put:' is: [ #at:put: printString ].
    t check: 'binary symbol printString' equals: '#+' is: [ #+ printString ].

    "--- size (number of characters, not keyword arg count) ---"
    t check: 'unary size' equals: 5 is: [ #hello size ].
    t check: 'keyword symbol size counts every char incl colons' equals: 7 is: [ #at:put: size ].
    t check: 'binary symbol size' equals: 1 is: [ #+ size ].
    t check: 'empty symbol size' equals: 0 is: [ '' asSymbol size ].
    t check: 'empty symbol isEmpty' that: [ '' asSymbol isEmpty ].
    t check: 'non-empty symbol notEmpty' that: [ #hello notEmpty ].

    "--- indexed access (read only) ---"
    t check: 'at: first char' equals: $h is: [ #hello at: 1 ].
    t check: 'at: last char' equals: $o is: [ #hello at: 5 ].
    t expect: 'at: past end raises OutOfRangeError' raises: OutOfRangeError in: [ | y | y := #hello. y at: 99 ].
    t expect: 'at: 0 raises OutOfRangeError' raises: OutOfRangeError in: [ | y | y := #hello. y at: 0 ].
    t check: 'first' equals: $h is: [ #hello first ].
    t check: 'last' equals: $o is: [ #hello last ].

    "--- inherited scanning / enumeration over the characters ---"
    t check: 'indexOf: finds first occurrence' equals: 3 is: [ #hello indexOf: $l ].
    t check: 'indexOf: absent is 0' equals: 0 is: [ #hello indexOf: $z ].
    t check: 'lastIndexOf:' equals: 6 is: [ #banana lastIndexOf: $a ].
    t check: 'occurrencesOf:' equals: 2 is: [ #hello occurrencesOf: $l ].
    t check: 'includes: present' that: [ #hello includes: $e ].
    t check: 'includes: absent' that: [ (#hello includes: $z) not ].
    t check: 'do: visits every character in order' that: [
        | acc | acc := OrderedCollection new.
        #abc do: [:ch | acc add: ch ].
        acc asArray = (Array with: $a with: $b with: $c) ].
    t check: 'inject:into: over characters' equals: 5 is: [ #hello inject: 0 into: [:n :ch | n + 1] ].
    t check: 'asArray of characters' that: [ #ab asArray = (Array with: $a with: $b) ].

    "--- hashing: equal (identical) symbols hash equal ---"
    t check: 'hash is stable/equal for the same interned symbol' that: [ #foo hash = #foo hash ].
    t check: 'interned-from-string hash matches literal' that: [ 'foo' asSymbol hash = #foo hash ].

    "--- immutability + string operations answer a plain String (species = String) ---"
    t expect: 'at:put: raises (symbols are immutable)' raises: Error in: [ | y | y := #foo. y at: 1 put: $X ].
    t check: 'asUppercase answers a String' that: [ #abc asUppercase class == String ].
    t check: 'asUppercase content' equals: 'HELLO' is: [ #Hello asUppercase ].
    t check: 'asLowercase content' equals: 'hello' is: [ #Hello asLowercase ].
    t check: 'reverse answers a String' that: [ #abc reverse class == String ].
    t check: 'reverse content' equals: 'cba' is: [ #abc reverse ].
    t check: 'copyFrom:to: answers a String' that: [ (#hello copyFrom: 1 to: 3) class == String ].
    t check: 'copyFrom:to: content' equals: 'hel' is: [ #hello copyFrom: 1 to: 3 ].
    t check: 'concat answers a String' equals: 'abcd' is: [ #ab , #cd ].

    "--- class membership ---"
    t check: 'a Symbol isKindOf: Symbol' that: [ #foo isKindOf: Symbol ].
    t check: 'a Symbol isKindOf: String (subclass)' that: [ #foo isKindOf: String ].
    t check: 'isKeyword true for trailing colon' that: [ #at: isKeyword ].
    t check: 'isKeyword false for plain word' that: [ #foo isKeyword not ].

    "--- selectors Symbol does NOT understand (documents the protocol boundary).
         Receiver bound to a temp to dodge the literal-receiver DNU segfault. ---"
    t check: 'asString answers a plain String' that: [ #foo asString class == String ].
    t check: 'asString content drops the #' equals: 'foo' is: [ #foo asString ].
    t check: 'asString is a fresh object, not the symbol' that: [ (#foo asString == #foo) not ].
    "Wave 3 gave String (and thus Symbol, its subclass) a lexicographic-by-byte
     ordering; this case used to assert < was absent (a DNU)."
    t check: 'Symbol inherits String lexicographic <' that: [ | x | x := #a. x < #b ].

    ^t report
]
