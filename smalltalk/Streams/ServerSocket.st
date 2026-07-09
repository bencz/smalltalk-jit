ServerSocket := Object [

	| descriptor |


	"instance creation"

	class bindTo: address port: port queueSize: queueSize [
		^self new initializeAddress: address port: port queueSize: queueSize
	]


	class bind: address port: port queueSize: queueSize [
		<primitive: SocketBindPrimitive>
		IoError last signal.
	]


	"initialization"

	initializeAddress: address port: port queueSize: queueSize [
		descriptor := self class bind: address port: port queueSize: queueSize.
	]


	accept [
		^Socket descriptor: self basicAccept
	]


	basicAccept [
		<primitive: SocketAcceptPrimitive>
		IoError last signal.
	]


	close [
		ExternalStream close: descriptor.
	]

]
