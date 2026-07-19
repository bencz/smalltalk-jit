"=== StringProtocolTest: concat, indexed access + mutability of copies,
 equality/identity/hashing, case folding, numeric parsing, scanning,
 enumeration, sub-collection copies, trimming and keyword parsing. Goes well
 beyond the existing StringTest.st (which only covers splitBy:/splitByAll:).

 STRUCTURE NOTE (VM constraint, not a logic choice): a single top-level block
 accumulates one runtime handle per string literal, and this VM hard-aborts at
 256 handles per scope (vm/core/Handle.h:175). This file has enough string
 literals to exceed that, so the checks are split across several top-level
 blocks. Each top-level block runs in a fresh handle scope, and they share one
 TestRun through the StrProtoSuite class variable below (a bare top-level
 assignment is a parse error, hence the tiny holder class).

 Observed VM behaviour these checks rely on:
  * Equal string literals are shared: `'abc' == 'abc'` is TRUE, but `copy` makes
    a distinct object, so identity checks use copy.
  * indexOf:ifAbsent: IGNORES its absent-block and always answers 0 (a kernel
    quirk in SequenceableCollection). The absent case is therefore checked via
    plain indexOf: returning 0, not via the block value.
  * Mutating a bare literal would corrupt every shared occurrence of it, so all
    at:put: mutation is done on an explicit copy. ==="
StrProtoSuite := Object [
    | Run |
    class run [ Run isNil ifTrue: [ Run := TestRun named: 'StringProtocolTest' ]. ^Run ]
]

"--- batch 1: concat, size, indexed read, copy mutability, equality, hashing ---"
[
    | t |
    t := StrProtoSuite run.

    t check: 'concat two non-empty' equals: 'abcdef' is: [ 'abc' , 'def' ].
    t check: 'empty on the left is identity of content' equals: 'abc' is: [ '' , 'abc' ].
    t check: 'empty on the right is identity of content' equals: 'abc' is: [ 'abc' , '' ].
    t check: 'concat answers a String' that: [ ('ab' , 'cd') class == String ].
    t check: 'concat length is the sum' equals: 5 is: [ ('ab' , 'cde') size ].

    t check: 'size of non-empty' equals: 5 is: [ 'hello' size ].
    t check: 'size of empty' equals: 0 is: [ '' size ].

    t check: 'at: first' equals: $h is: [ 'hello' at: 1 ].
    t check: 'at: last' equals: $o is: [ 'hello' at: 5 ].
    t check: 'first' equals: $h is: [ 'hello' first ].
    t check: 'last' equals: $o is: [ 'hello' last ].
    t expect: 'at: past end raises OutOfRangeError' raises: OutOfRangeError in: [ | s | s := 'abc'. s at: 9 ].
    t expect: 'at: 0 raises OutOfRangeError' raises: OutOfRangeError in: [ | s | s := 'abc'. s at: 0 ].
    t expect: 'first of empty raises OutOfRangeError' raises: OutOfRangeError in: [ | s | s := ''. s first ].

    t check: 'copy is writable via at:put:' equals: 'Xbc' is: [ | s | s := 'abc' copy. s at: 1 put: $X. s ].
    t check: 'mutating the copy leaves the original content intact' equals: 'abc' is: [
        | orig cp | orig := 'abc' copy. cp := orig copy. cp at: 1 put: $Z. orig ].
    t check: 'copy is not identical to the original' that: [ ('abc' copy == 'abc') not ].
    t check: 'copy is equal to the original' that: [ 'abc' copy = 'abc' ].

    t check: 'equal content' that: [ 'abc' = 'abc' ].
    t check: 'different content' that: [ ('abc' = 'abd') not ].
    t check: 'different length' that: [ ('abc' = 'ab') not ].
    t check: '~= for different content' that: [ 'abc' ~= 'abd' ].

    t check: 'equal strings hash equal' that: [ 'hello' hash = ('hel' , 'lo') hash ].
    t check: 'empty strings hash equal' that: [ '' hash = '' hash ].
    t check: 'copy hashes equal to original' that: [ 'hello' copy hash = 'hello' hash ].
]

"--- batch 2: case folding, conversions, reverse, scanning ---"
[
    | t |
    t := StrProtoSuite run.

    t check: 'asUppercase' equals: 'HELLO WORLD' is: [ 'Hello World' asUppercase ].
    t check: 'asLowercase' equals: 'hello world' is: [ 'Hello World' asLowercase ].
    t check: 'asUppercase is idempotent' equals: 'ABC' is: [ 'ABC' asUppercase ].
    t check: 'asLowercase of empty' equals: '' is: [ '' asLowercase ].
    t check: 'asUppercase leaves digits/punctuation alone' equals: 'A1!' is: [ 'a1!' asUppercase ].

    t check: 'asSymbol interns to the literal symbol' that: [ 'foo' asSymbol == #foo ].
    t check: 'asSymbol answers a Symbol' that: [ 'foo' asSymbol class == Symbol ].
    t check: 'asNumber positive integer' equals: 123 is: [ '123' asNumber ].
    t check: 'asNumber negative integer' equals: -42 is: [ '-42' asNumber ].
    t check: 'asNumber float' equals: 3.14 is: [ '3.14' asNumber ].
    t check: 'asNumber of empty is 0' equals: 0 is: [ '' asNumber ].
    t check: 'asNumber of non-numeric is 0' equals: 0 is: [ 'abc' asNumber ].

    t check: 'reverse' equals: 'cba' is: [ 'abc' reverse ].
    t check: 'reversed alias' equals: 'cba' is: [ 'abc' reversed ].
    t check: 'reverse of a palindrome' equals: 'level' is: [ 'level' reverse ].
    t check: 'reverse answers a String' that: [ 'abc' reverse class == String ].

    t check: 'indexOf: first occurrence' equals: 2 is: [ 'banana' indexOf: $a ].
    t check: 'indexOf: absent is 0' equals: 0 is: [ 'banana' indexOf: $z ].
    t check: 'indexOf:ifAbsent: with present element' equals: 3 is: [ 'hello' indexOf: $l ifAbsent: [ -1 ] ].
    t check: 'lastIndexOf:' equals: 6 is: [ 'banana' lastIndexOf: $a ].
    t check: 'occurrencesOf: repeated' equals: 3 is: [ 'banana' occurrencesOf: $a ].
    t check: 'occurrencesOf: absent is 0' equals: 0 is: [ 'banana' occurrencesOf: $z ].
    t check: 'includes: present' that: [ 'hello' includes: $e ].
    t check: 'includes: absent' that: [ ('hello' includes: $z) not ].
    t check: 'indexOfSubCollection:startingAt: found' equals: 7 is: [ 'hello world' indexOfSubCollection: 'wor' startingAt: 1 ].
    t check: 'indexOfSubCollection:startingAt: absent is 0' equals: 0 is: [ 'hello world' indexOfSubCollection: 'xyz' startingAt: 1 ].
]

"--- batch 3: startsWith, ascii-case compare, sub-collection copies, enumeration ---"
[
    | t |
    t := StrProtoSuite run.

    t check: 'startsWith: prefix' that: [ 'hello' startsWith: 'he' ].
    t check: 'startsWith: empty is true' that: [ 'hello' startsWith: '' ].
    t check: 'startsWith: whole string' that: [ 'hello' startsWith: 'hello' ].
    t check: 'startsWith: non-prefix is false' that: [ ('hello' startsWith: 'xe') not ].
    t check: 'startsWith: longer than self is false' that: [ ('he' startsWith: 'hello') not ].

    t check: 'sameAsciiCaseAs: mixed case equal' that: [ 'Hello' sameAsciiCaseAs: 'hELLO' ].
    t check: 'sameAsciiCaseAs: different length false' that: [ ('Hello' sameAsciiCaseAs: 'Hell') not ].
    t check: 'sameAsciiCaseAs: different letters false' that: [ ('Hello' sameAsciiCaseAs: 'Jello') not ].

    t check: 'copyFrom:to:' equals: 'hello' is: [ 'hello world' copyFrom: 1 to: 5 ].
    t check: 'copyFrom: to end' equals: 'world' is: [ 'hello world' copyFrom: 7 ].
    t check: 'copyFrom:to: answers a String' that: [ ('hello' copyFrom: 2 to: 4) class == String ].
    t check: 'copyReplaceFrom:to:with:' equals: 'Jello' is: [ 'hello' copyReplaceFrom: 1 to: 1 with: 'J' ].
    t check: 'copyWith: appends one char' equals: 'abcd' is: [ 'abc' copyWith: $d ].

    t check: 'do: visits each char' equals: 5 is: [ | n | n := 0. 'hello' do: [:ch | n := n + 1]. n ].
    t check: 'collect: answers a String' that: [ ('abc' collect: [:ch | ch asUppercase]) = 'ABC' ].
    t check: 'select: keeps matching chars (as OrderedCollection)' that: [
        ('hello' select: [:ch | ch isVowel]) asArray = (Array with: $e with: $o) ].
    t check: 'reject: drops matching chars' that: [
        ('hello' reject: [:ch | ch isVowel]) asArray = (Array with: $h with: $l with: $l) ].
    t check: 'inject:into: counts characters' equals: 5 is: [ 'hello' inject: 0 into: [:n :ch | n + 1] ].
    t check: 'detect:ifNone: finds first vowel' equals: $e is: [ 'hello' detect: [:ch | ch isVowel] ifNone: [ $? ] ].
    t check: 'detect:ifNone: falls back when none match' equals: $? is: [ 'xyz' detect: [:ch | ch isVowel] ifNone: [ $? ] ].
]

"--- batch 4: collection conversions, trimming, splitting, keywords, boundary ---"
[
    | t |
    t := StrProtoSuite run.

    t check: 'asArray of characters' that: [ 'ab' asArray = (Array with: $a with: $b) ].
    t check: 'asOrderedCollection size' equals: 3 is: [ 'abc' asOrderedCollection size ].
    t check: 'asSet folds duplicate chars' equals: 3 is: [ 'banana' asSet size ].
    t check: 'asSortedCollection orders chars' that: [ 'dbca' asSortedCollection asArray = (Array with: $a with: $b with: $c with: $d) ].

    t check: 'trimSeparators strips both ends' equals: 'hi there' is: [ '  hi there  ' trimSeparators ].
    t check: 'trimSeparators of all-blank is empty' equals: '' is: [ '     ' trimSeparators ].
    t check: 'trimSeparators of already-trimmed is unchanged' equals: 'x' is: [ 'x' trimSeparators ].
    t check: 'trimSeparators keeps interior separators' equals: 'a b' is: [ ' a b ' trimSeparators ].

    t check: 'splitBy: a comma' that: [ ('a,b,c' splitBy: $,) asArray = (Array with: 'a' with: 'b' with: 'c') ].
    t check: 'splitBy: collapses empty segments' that: [ ('a,,b' splitBy: $,) asArray = (Array with: 'a' with: 'b') ].
    t check: 'splitByAll: over a set of delimiter chars' that: [ ('a-b_c' splitByAll: '-_') asArray = (Array with: 'a' with: 'b' with: 'c') ].

    t check: 'keywords of a keyword string' that: [ 'at:put:' keywords asArray = (Array with: 'at:' with: 'put:') ].
    t check: 'keywords of a plain word is the word itself' that: [ 'foo' keywords asArray = (Array with: 'foo') ].
    t check: 'isKeyword true when ending in colon' that: [ 'at:' isKeyword ].
    t check: 'isKeyword false for plain word' that: [ 'foo' isKeyword not ].

    t check: 'empty isEmpty' that: [ '' isEmpty ].
    t check: 'non-empty notEmpty' that: [ 'a' notEmpty ].
    t check: 'non-empty is not isEmpty' that: [ 'a' isEmpty not ].

    "Wave 3 gave String a lexicographic-by-byte ordering (it is still not a
     Magnitude; the whole comparison protocol is defined directly on String).
     These two cases used to assert the ordering was absent (a DNU)."
    t check: 'lexicographic < true' that: [ | s | s := 'abc'. s < 'abd' ].
    t check: 'lexicographic <= reflexive' that: [ | s | s := 'abc'. s <= 'abc' ].
]

"--- final block: emit the report; its integer return is the process exit code ---"
[ ^StrProtoSuite run report ]
