Error := Exception [

	"Errors are not resumable: the signaling code cannot be meaningfully
	 continued past the failure point (ANSI). resume: on an Error raises."
	isResumable [
		^false
	]

]
