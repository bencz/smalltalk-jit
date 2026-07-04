"============================================================
  ADVANCED - Command Pattern (with undo)
  ------------------------------------------------------------
  Encapsulate an action as an object that knows how to #execute
  itself and how to #undo itself. Because commands are objects,
  the receiver can keep a history and replay it backwards to
  implement undo.

  Example: a tiny text editor supporting append, clear and undo.
  ============================================================"

Editor := Object [
	| text history |

	class new [ ^super new initEditor ]
	initEditor [ text := ''. history := OrderedCollection new ]

	text [ ^text ]
	setText: aString [ text := aString ]

	"Run a command and remember it so it can be undone."
	do: aCommand [
		aCommand executeOn: self.
		history addLast: aCommand.
	]

	undo [
		history isEmpty
			ifTrue:  [Transcript nextPutAll: '  (nothing to undo)'; lf]
			ifFalse: [history removeLast undoOn: self]
	]

	show [
		Transcript nextPutAll: '  text = "'; nextPutAll: text; nextPutAll: '"'; lf.
	]
]

Command := Object [
	executeOn: anEditor [ ^self subclassResponsibility ]
	undoOn: anEditor [ ^self subclassResponsibility ]
]

AppendCommand := Command [
	| appended |
	class text: aString [ ^self new setText: aString ]
	setText: aString [ appended := aString ]

	executeOn: editor [ editor setText: editor text , appended ]

	undoOn: editor [
		| t |
		t := editor text.
		editor setText: (t copyFrom: 1 to: t size - appended size).
	]
]

ClearCommand := Command [
	| saved |
	executeOn: editor [ saved := editor text. editor setText: '' ]
	undoOn: editor [ editor setText: saved ]
]

[
	| editor |
	editor := Editor new.

	editor do: (AppendCommand text: 'Hello').           editor show.
	editor do: (AppendCommand text: ', World').          editor show.
	editor do: (AppendCommand text: '!').                editor show.
	editor do: ClearCommand new.                         editor show.

	Transcript nextPutAll: 'undo clear:'; lf.  editor undo. editor show.
	Transcript nextPutAll: 'undo "!":'; lf.    editor undo. editor show.
	Transcript nextPutAll: 'undo ", World":'; lf. editor undo. editor show.
]
