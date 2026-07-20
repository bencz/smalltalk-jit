"File: filesystem operations on a regular-file path String. Class side only.
 Metadata comes from the stat primitive; contents go through FileStream. All
 mutating operations answer true/false so callers never have to interpret a
 primitive failure."

File := Object [

	"--- testing and metadata ---"

	class exists: aPath [
		"True when aPath exists and is a regular file."
		| stat |

		stat := self statPath: aPath.
		^stat notNil and: [(stat at: 1) = 1]
	]


	class size: aPath [
		"The file size in bytes, or nil when aPath does not exist."
		| stat |

		stat := self statPath: aPath.
		^stat isNil ifTrue: [nil] ifFalse: [stat at: 2]
	]


	class modificationTimeMillis: aPath [
		"The last-modified time in milliseconds since the epoch, or nil when the
		 path is missing."
		| stat |

		stat := self statPath: aPath.
		^stat isNil ifTrue: [nil] ifFalse: [stat at: 3]
	]


	class realpath: aPath [
		"The canonical absolute path (symlinks and dot segments resolved), or
		 the path unchanged when the OS cannot resolve it."
		<primitive: FileRealpathPrimitive>
		^aPath
	]


	class modificationTime: aPath [
		"The last-modified time as a DateTime, or nil when the path is missing."
		| ms |

		ms := self modificationTimeMillis: aPath.
		^ms isNil ifTrue: [nil] ifFalse: [DateTime fromMilliseconds: ms]
	]


	"--- mutating ---"

	class delete: aPath [
		"Unlink aPath. Answer true on success, false on failure."
		^self unlinkPath: aPath
	]


	class rename: fromPath to: toPath [
		"Rename fromPath to toPath. Answer true on success, false on failure."
		^self renamePath: fromPath to: toPath
	]


	"--- contents ---"

	class readAll: aPath [
		"Open aPath for reading, answer its entire contents as a String, and close
		 the stream even if reading is curtailed. The whole file is read in a
		 single request sized from the stat: BufferedStream>>contents cannot be
		 used because it pads its answer to the internal 2048-byte buffer size.
		 A missing path lets FileStream read: raise IoError before we get here."
		| stream size |

		stream := FileStream read: aPath.
		^[size := self size: aPath.
		  (size isNil or: [size = 0]) ifTrue: [''] ifFalse: [stream next: size]]
			ensure: [stream close]
	]


	class writeAll: aString to: aPath [
		"Create (or truncate) aPath and write aString to it, flushing and closing
		 the stream. Answer aString."
		| stream |

		stream := FileStream write: aPath.
		[stream nextPutAll: aString. stream flush] ensure: [stream close].
		^aString
	]


	"--- primitive wrappers (private) ---"

	class statPath: aString [
		<primitive: FileStatPrimitive>
	]


	class unlinkPath: aString [
		<primitive: FileUnlinkPrimitive>
	]


	class renamePath: fromString to: toString [
		<primitive: FileRenamePrimitive>
	]

]
