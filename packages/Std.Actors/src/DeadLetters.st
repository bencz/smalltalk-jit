"Where messages go when their target Pid has no live actor (stopped, never
 existed, or already terminated). For v1 this simply reports them; later it can
 publish to an event stream that subscribers (metrics, logging) can observe."
DeadLetterProcess := Object [

	postUser: aMessage sender: aSenderPid [
		Transcript nextPutAll: '[dead-letter] undeliverable message dropped'; lf.
		^self
	]

	postSystem: aMessage [
		^self
	]
]
