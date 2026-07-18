"=== CharacterProtocolTest: code points and round-tripping, class-side
 constants and constructors, digit values, the isX classification predicates,
 case folding, Magnitude ordering, identity/hashing, and printOn: rendering.
 Goes well beyond the existing CharacterTest.st (a single isSeparator check).

 Facts pinned by these checks: Characters are immediate values, so equal
 characters are also identical (==); asInteger/value do NOT exist (the accessor
 is codePoint); the constructor is `Character codePoint:` (there is no
 `Character value:`); `Character new` is disallowed. ==="
[
    | t |
    t := TestRun named: 'CharacterProtocolTest'.

    "--- code points ---"
    t check: 'codePoint of a lowercase letter' equals: 97 is: [ $a codePoint ].
    t check: 'codePoint of an uppercase letter' equals: 65 is: [ $A codePoint ].
    t check: 'codePoint of a digit char' equals: 48 is: [ $0 codePoint ].

    "--- class-side construction and round-trip ---"
    t check: 'Character codePoint: builds the char' equals: $a is: [ Character codePoint: 97 ].
    t check: 'codePoint round-trips through the constructor' that: [ (Character codePoint: $z codePoint) = $z ].
    t check: 'constructor and literal are identical (immediates)' that: [ (Character codePoint: 65) == $A ].
    t expect: 'Character new is disallowed' raises: ShouldNotImplement in: [ Character new ].

    "--- class-side untypeable constants ---"
    t check: 'cr codePoint is 13' equals: 13 is: [ Character cr codePoint ].
    t check: 'lf codePoint is 10' equals: 10 is: [ Character lf codePoint ].
    t check: 'tab codePoint is 9' equals: 9 is: [ Character tab codePoint ].
    t check: 'space codePoint is 32' equals: 32 is: [ Character space codePoint ].

    "--- digit values ---"
    t check: 'digitValue of $0' equals: 0 is: [ $0 digitValue ].
    t check: 'digitValue of $9' equals: 9 is: [ $9 digitValue ].
    t check: 'digitValue of $A (base>10)' equals: 10 is: [ $A digitValue ].
    t check: 'digitValue of $F' equals: 15 is: [ $F digitValue ].
    t check: 'digitValue of $Z' equals: 35 is: [ $Z digitValue ].
    t expect: 'digitValue of a non-digit raises Error' raises: Error in: [ | c | c := Character space. c digitValue ].
    t check: 'class-side digitValue: 10 answers $A' equals: $A is: [ Character digitValue: 10 ].
    t check: 'class-side digitValue: 15 answers $F' equals: $F is: [ Character digitValue: 15 ].
    t check: 'class-side digitValue: 0 answers $0' equals: $0 is: [ Character digitValue: 0 ].

    "--- isDigit / isDigit: (radix) ---"
    t check: 'isDigit true for a decimal digit' that: [ $5 isDigit ].
    t check: 'isDigit false for a letter' that: [ $a isDigit not ].
    t check: 'isDigit false for punctuation' that: [ $! isDigit not ].
    t check: 'isDigit: 16 true for hex letter $A' that: [ $A isDigit: 16 ].
    t check: 'isDigit: 16 false for $G' that: [ ($G isDigit: 16) not ].
    t check: 'isDigit: 10 false for a letter' that: [ ($a isDigit: 10) not ].

    "--- isLetter / isVowel ---"
    t check: 'isLetter true for lowercase' that: [ $a isLetter ].
    t check: 'isLetter true for uppercase' that: [ $Z isLetter ].
    t check: 'isLetter false for a digit' that: [ $1 isLetter not ].
    t check: 'isLetter false for punctuation' that: [ $! isLetter not ].
    t check: 'isVowel true for $a' that: [ $a isVowel ].
    t check: 'isVowel true for $e' that: [ $e isVowel ].
    t check: 'isVowel true for uppercase $A' that: [ $A isVowel ].
    t check: 'isVowel false for $b' that: [ $b isVowel not ].

    "--- isSeparator (beyond CharacterTest.st: also tab/lf/cr/formfeed and a non-separator) ---"
    t check: 'space isSeparator' that: [ Character space isSeparator ].
    t check: 'tab isSeparator' that: [ Character tab isSeparator ].
    t check: 'lf isSeparator' that: [ Character lf isSeparator ].
    t check: 'cr isSeparator' that: [ Character cr isSeparator ].
    t check: 'formfeed (code 12) isSeparator' that: [ (Character codePoint: 12) isSeparator ].
    t check: 'a letter is not a separator' that: [ $a isSeparator not ].

    "--- isSpecial / isAlphaNumeric ---"
    t check: 'isSpecial true for $+' that: [ $+ isSpecial ].
    t check: 'isSpecial false for a letter' that: [ $a isSpecial not ].
    t check: 'isAlphaNumeric true for a letter' that: [ $a isAlphaNumeric ].
    t check: 'isAlphaNumeric true for a digit' that: [ $5 isAlphaNumeric ].
    t check: 'isAlphaNumeric false for punctuation' that: [ $! isAlphaNumeric not ].
    t check: 'isAlphaNumeric false for space' that: [ Character space isAlphaNumeric not ].

    "--- upper/lower testing and folding ---"
    t check: 'isUppercase true for $A' that: [ $A isUppercase ].
    t check: 'isUppercase false for $a' that: [ $a isUppercase not ].
    t check: 'isLowercase true for $a' that: [ $a isLowercase ].
    t check: 'isLowercase false for $A' that: [ $A isLowercase not ].
    t check: 'asUppercase folds lowercase' equals: $A is: [ $a asUppercase ].
    t check: 'asUppercase is idempotent' equals: $A is: [ $A asUppercase ].
    t check: 'asLowercase folds uppercase' equals: $z is: [ $Z asLowercase ].
    t check: 'asLowercase is idempotent' equals: $a is: [ $a asLowercase ].
    t check: 'asUppercase leaves a digit unchanged' equals: $5 is: [ $5 asUppercase ].
    t check: 'asLowercase leaves punctuation unchanged' equals: $! is: [ $! asLowercase ].

    "--- Magnitude ordering (Character IS a Magnitude) ---"
    t check: '$a < $b' that: [ $a < $b ].
    t check: '$b < $a is false' that: [ ($b < $a) not ].
    t check: '$a <= $a' that: [ $a <= $a ].
    t check: '$z > $a' that: [ $z > $a ].
    t check: '$a >= $a' that: [ $a >= $a ].
    t check: 'between:and: inside the range' that: [ $c between: $a and: $z ].
    t check: 'between:and: outside the range' that: [ ($A between: $a and: $z) not ].
    t check: 'max: answers the greater' equals: $z is: [ $a max: $z ].
    t check: 'min: answers the lesser' equals: $a is: [ $c min: $a ].
    t check: 'ordering follows code points across the alphabet' that: [
        ($a < $b) and: [ ($b < $c) and: [ $y < $z ] ] ].
    t check: 'digits sort before uppercase letters' that: [ $9 < $A ].
    t check: 'uppercase letters sort before lowercase (ASCII)' that: [ $Z < $a ].

    "--- identity, equality, hashing (immediates) ---"
    t check: 'equal chars are equal' that: [ $a = $a ].
    t check: 'different chars are not equal' that: [ ($a = $b) not ].
    t check: 'equal chars are identical (immediate)' that: [ $a == $a ].
    t check: 'constant space is identical to itself' that: [ Character space == Character space ].
    t check: 'cr is not equal to lf' that: [ (Character cr = Character lf) not ].
    t check: 'equal chars hash equal' that: [ $a hash = $a hash ].
    t check: 'different chars usually hash differently' that: [ ($a hash = $b hash) not ].

    "--- printOn: rendering ---"
    t check: 'printString of a printable char' equals: '$a' is: [ $a printString ].
    t check: 'printString of an uppercase char' equals: '$Z' is: [ $Z printString ].
    t check: 'printString of a digit char' equals: '$5' is: [ $5 printString ].
    t check: 'printString of space' equals: '$<SP>' is: [ Character space printString ].
    t check: 'printString of cr' equals: '$<CR>' is: [ Character cr printString ].
    t check: 'printString of lf' equals: '$<LF>' is: [ Character lf printString ].
    t check: 'printString of tab renders its code' equals: '$<9>' is: [ Character tab printString ].

    ^t report
]
