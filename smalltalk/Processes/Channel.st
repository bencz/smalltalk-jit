"An unbounded blocking channel (a la Go channels / actor mailboxes). Senders
 never block; a receiver blocks until an item is available. Safe for multiple
 producers and consumers under the cooperative scheduler."
Channel := Object [

	| queue itemsAvailable |


	class new [
		^super new initChannel
	]


	initChannel [
		queue := OrderedCollection new.
		itemsAvailable := Semaphore new.
		^self
	]


	"sending"

	send: anObject [
		queue addLast: anObject.
		itemsAvailable signal.
		^anObject
	]


	"receiving"

	receive [
		"Block the current process until an item is available, then return it."
		itemsAvailable wait.
		^queue removeFirst
	]


	"testing"

	isEmpty [
		^queue isEmpty
	]


	size [
		^queue size
	]

]
