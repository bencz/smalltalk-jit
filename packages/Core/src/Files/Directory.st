"Directory: directory-level filesystem operations on a path String. Class side
 only. entries: lists bare names (the primitive already drops '.' and '..');
 filesIn:/directoriesIn: answer full joined paths filtered by stat type."

Directory := Object [

	"--- testing ---"

	class exists: aPath [
		"True when aPath exists and is a directory."
		| stat |

		stat := self statPath: aPath.
		^stat notNil and: [(stat at: 1) = 2]
	]


	"--- mutating ---"

	class create: aPath [
		"Create a single directory. Answer true on success, false on failure."
		^self mkdirPath: aPath
	]


	class delete: aPath [
		"Remove an empty directory. Answer true on success, false on failure."
		^self rmdirPath: aPath
	]


	"--- listing ---"

	class entries: aPath [
		"An OrderedCollection of the entry names in aPath (excluding '.' and
		 '..'), or nil when aPath is not a readable directory."
		^self listDirPath: aPath
	]


	class filesIn: aPath [
		"The entries of aPath that are regular files, as full joined paths, or nil
		 when aPath is not a readable directory."
		^self entriesOfType: 1 in: aPath
	]


	class directoriesIn: aPath [
		"The entries of aPath that are subdirectories, as full joined paths, or nil
		 when aPath is not a readable directory."
		^self entriesOfType: 2 in: aPath
	]


	class current [
		"The process working directory as a String."
		^self getCwd
	]


	"--- private ---"

	class entriesOfType: aType in: aPath [
		| names result full stat |

		names := self entries: aPath.
		names isNil ifTrue: [^nil].
		result := OrderedCollection new.
		names do: [ :name |
			full := Path join: aPath with: name.
			stat := self statPath: full.
			(stat notNil and: [(stat at: 1) = aType]) ifTrue: [result add: full]].
		^result
	]


	"--- primitive wrappers (private) ---"

	class statPath: aString [
		<primitive: FileStatPrimitive>
	]


	class mkdirPath: aString [
		<primitive: FileMkdirPrimitive>
	]


	class rmdirPath: aString [
		<primitive: FileRmdirPrimitive>
	]


	class listDirPath: aString [
		<primitive: FileListDirPrimitive>
	]


	class getCwd [
		<primitive: FileGetCwdPrimitive>
	]

]
