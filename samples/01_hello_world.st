"============================================================
  01 - Hello, World!
  ------------------------------------------------------------
  The classics: printing text, string concatenation, and the
  cascade operator (;) which sends several messages to the
  same receiver.
  ============================================================"

"A top-level statement list lives inside a [ ... ] block that the
 VM evaluates when the file is loaded."
[
	"'Transcript' is the standard output stream.
	 nextPutAll: writes a String, lf writes a line feed."
	Transcript nextPutAll: 'Hello, World!'; lf.

	"Strings are concatenated with the , (comma) binary message."
	Transcript nextPutAll: 'Smalltalk' , ' is ' , 'fun!'; lf.
]

"printNl prints any object's printString followed by a newline.
 Notice how a String prints *with* quotes, while a number does not."
[
	'a bare string prints with quotes' printNl.
	42 printNl.
	$A printNl.
	#aSymbol printNl.
	true printNl.
	nil printNl.
]

"A cascade (;) keeps sending to the first receiver (Transcript here),
 so this whole banner is one expression."
[
	Transcript
		nextPutAll: '+------------------------+'; lf;
		nextPutAll: '|  Welcome to Smalltalk  |'; lf;
		nextPutAll: '+------------------------+'; lf.
]
