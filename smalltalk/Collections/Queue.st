"A thin FIFO facade over OrderedCollection. Elements enter at the back and
 leave from the front, so enqueue:/dequeue/peek map onto addLast:/removeFirst/
 first."

Queue := OrderedCollection [

	enqueue: anObject [
		^self addLast: anObject
	]


	dequeue [
		^self removeFirst
	]


	peek [
		^self first
	]

]
