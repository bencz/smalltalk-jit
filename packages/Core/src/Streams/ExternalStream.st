ExternalStream := BufferedStream [

	| descriptor |


	"class initialization"

	class initialize [
		Transcript := self descriptor: 1.
		"Standard error: the sibling of Transcript (descriptor 1) over fd 2,
		 created here the same way so both globals exist right after bootstrap.
		 System class stderr answers this same object."
		StdErr := self descriptor: 2.
	]


	"instance creation"

	class descriptor: anInteger [
		^self new initializeDescriptor: anInteger
	]


	"initialization"

	initializeDescriptor: anInteger [
		descriptor := anInteger.
		self initialize.
	]


	createBuffer [
		^String new: 2048
	]


	"IO primitives"

	class read: descriptor next: anInteger into: aString startingAt: start [
		<primitive: StreamReadPrimitive>
		IoError last signal.
	]


	class write: descriptor next: anInteger from: aString [
		<primitive: StreamWritePrimitive>
		IoError last signal.
	]


	class close: descriptor [
		<primitive: StreamClosePrimitive>
		IoError last signal.
	]


	class flush: descriptor [
		<primitive: StreamFlushPrimitive>
		IoError last signal.
	]


	class position: descriptor [
		<primitive: StreamGetPositionPrimitive>
		IoError last signal.
	]


	class position: descriptor to: anInteger[
		<primitive: StreamSetPositionPrimitive>
		IoError last signal.
	]


	class available: descriptor [
		<primitive: StreamAvailablePrimitive>
		IoError last signal.
	]


	"accessing"

	nextPut: aCharacter [
		buffer at: 1 put: aCharacter.
		self class write: descriptor next: 1 from: buffer.
		"self flush."
	]


	nextPutAll: aCollection [
		self class write: descriptor next: aCollection size from: aCollection.
	]


	"accessing private"

	directNext: anInteger into: aCollection startingAt: start [
		| read lastRead |

		read := 0.
		[lastRead := self class read: descriptor next: anInteger into: aCollection startingAt: start.
		read := read + lastRead.
		read < anInteger and: [lastRead > 0]] whileTrue.

		^read
	]


	"positioning"

	position [
		^(self class position: descriptor) - buffered + 1
	]


	position: anInteger [
		self class position: descriptor to: anInteger - 1.
		position := buffer size + 1.
		buffered := 0.
	]


	"flushing"

	flush [
		self class flush: descriptor.
	]


	"closing"

	close [
		"Idempotent: closing an already-closed stream (e.g. when the finalizer
		 runs after an explicit close) must not re-close a stale descriptor."
		descriptor isNil ifTrue: [ ^self ].
		self class close: descriptor.
		descriptor := nil.
	]


	finalize [
		self close.
	]


	species [
		^buffer class
	]

]
