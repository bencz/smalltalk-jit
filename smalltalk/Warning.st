"A resumable notification. Unhandled, it prints its message and resumes nil:
 the default action's answer becomes the value of the signal expression and
 execution simply continues at the signal point (no handler means no unwind
 ever happened)."

Warning := Exception [

	defaultAction [
		Transcript nextPutAll: self messageText; lf.
		^nil
	]

]
