Socket := ExternalStream [

	"instance creation"

	class connectTo: address port: port [
		^self new initializeAddress: address port: port
	]


	class connect: address port: port [
		<primitive: SocketConnectPrimitive>
	]


	"initialization"

	initializeAddress: address port: port [
		self initializeDescriptor: (self class connect: address port: port).
	]


	"non-blocking IO primitives (park the fiber until the socket is ready)"

	class read: descriptor into: aString size: anInteger startingAt: start [
		<primitive: SocketReadPrimitive>
		IoError last signal.
	]


	class write: descriptor from: aString size: anInteger [
		<primitive: SocketWritePrimitive>
		IoError last signal.
	]


	"accessing private"

	directNext: anInteger into: aCollection startingAt: start [
		"Read whatever is available (parking until at least one byte arrives);
		 a result of 0 means the peer closed the connection."
		^Socket read: descriptor into: aCollection size: anInteger startingAt: start
	]


	"writing"

	nextPut: aCharacter [
		buffer at: 1 put: aCharacter.
		Socket write: descriptor from: buffer size: 1.
	]


	nextPutAll: aCollection [
		Socket write: descriptor from: aCollection size: aCollection size.
		^aCollection
	]


	"flushing"

	flush [

	]


	"testing"

	atEnd [
		^false
	]

]
