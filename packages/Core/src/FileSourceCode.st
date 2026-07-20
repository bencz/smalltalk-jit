FileSourceCode := SourceCode [

	"accessing"

	sourceContents [
		"The method's source text, re-read lazily from its file. A built
		 project image can run on a machine WITHOUT the sources, so a missing
		 or unreadable file degrades to a placeholder instead of raising from
		 inside the debugger or a backtrace printer."
		| stream |

		^[
			stream := FileStream read: self source.
			stream position: self position.
			stream next: self sourceSize
		] on: Error do: [ :e | '<source unavailable: ', self source, '>' ]
	]

]
