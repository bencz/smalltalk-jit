IoError := Error [

	"instance creation"

	class last [
		"The last OS error, as an exception carrying its message.

		 The fallback is not decoration: a method whose body is EMPTY answers
		 SELF, so without it this answered the IoError CLASS, and `IoError last
		 signal` then went looking for a class-side signal. Reporting the failure
		 needed the Transcript, writing to the Transcript failed, and the two
		 recursed until the stack ran out. Measured."
		<primitive: LastIoErrorPrimitive>
		^self new messageText: 'input/output error'; yourself
	]


	appendName: aString [
		| stream |

		stream := CollectionStream with: (String new: self messageText size + 3 + aString size).
		stream nextPutAll: self messageText;
			nextPut: Character space.
		aString printOn: stream.
		self messageText: stream contents.
	]

]
