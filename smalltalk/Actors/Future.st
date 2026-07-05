"A Future is the reply side of request/response. It registers itself as an
 endpoint so it has a real Pid that can be used as the sender of a request; when
 the target actor responds, the reply is delivered here and completes the
 Future, waking whoever is awaiting it."
Future := Object [

	| system pid channel completed value |


	class in: aSystem [
		^self new initIn: aSystem
	]


	initIn: aSystem [
		system := aSystem.
		channel := Channel new.
		completed := false.
		pid := system registerEndpoint: self.
		^self
	]


	pid [ ^pid ]


	"endpoint protocol (the system delivers replies through this)"

	postUser: aMessage sender: aSenderPid [
		self complete: aMessage.
		^self
	]

	postSystem: aMessage [ ^self ]


	"completion"

	complete: aValue [
		completed ifTrue: [ ^self ].
		completed := true.
		value := aValue.
		system unregister: pid.
		channel send: aValue.
		^self
	]


	"awaiting"

	await [
		"Block the current process until the reply arrives."
		completed ifTrue: [ ^value ].
		^channel receive
	]


	awaitFor: milliseconds [
		"Block until the reply arrives or the timeout elapses (then answer nil)."
		completed ifTrue: [ ^value ].
		[ (Delay forMilliseconds: milliseconds) wait.
		  completed ifFalse: [ channel send: nil ] ] fork.
		^channel receive
	]
]
