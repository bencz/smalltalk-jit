"============================================================
  06 - Strings and Characters
  ------------------------------------------------------------
  A String is a collection of Characters, so every enumeration
  message works on it. Here we build a few small text utilities.
  ============================================================"

"---- Characters ----"
[
	Transcript nextPutAll: '$A codePoint  = '. $A codePoint printNl.
	Transcript nextPutAll: 'code 97 char  = '. (Character codePoint: 97) printNl.
	Transcript nextPutAll: '$e isVowel    = '. $e isVowel printNl.
	Transcript nextPutAll: '$7 isDigit    = '. $7 isDigit printNl.
	Transcript nextPutAll: '$7 digitValue = '. $7 digitValue printNl.
]

"---- Upper / lower case via collect: ----"
[
	| shout |
	shout := 'hello' collect: [:c | c asUppercase].
	Transcript nextPutAll: 'shout = '; nextPutAll: shout; lf.
	Transcript nextPutAll: 'quiet = '; nextPutAll: ('LOUD' collect: [:c | c asLowercase]); lf.
]

"---- Reversing a string (build with a stream) ----"
[
	| src stream |
	src := 'Smalltalk'.
	stream := CollectionStream with: (String new: 0).
	src reverseDo: [:c | stream nextPut: c].
	Transcript nextPutAll: 'reversed = '; nextPutAll: stream contents; lf.
]

"---- Palindrome test ----"
[
	| isPalindrome |
	isPalindrome := [:word |
		| s |
		s := CollectionStream with: (String new: 0).
		word reverseDo: [:c | s nextPut: c].
		s contents = word].

	#('racecar' 'level' 'smalltalk' 'noon') do: [:w |
		Transcript nextPutAll: '  '; nextPutAll: w; nextPutAll: ' -> '.
		(isPalindrome value: w) printNl].
]

"---- Counting vowels with inject:into: ----"
[
	| text count |
	text := 'the quick brown fox'.
	count := text inject: 0 into: [:acc :c |
		c isVowel ifTrue: [acc + 1] ifFalse: [acc]].
	Transcript nextPutAll: 'vowels in "'; nextPutAll: text; nextPutAll: '" = '.
	count printNl.
]

"---- Splitting into words ----"
[
	| words |
	words := 'the quick brown fox' splitBy: Character space.
	Transcript nextPutAll: 'word count = '. words size printNl.
	Transcript nextPutAll: 'words: '. words printNl.
]
