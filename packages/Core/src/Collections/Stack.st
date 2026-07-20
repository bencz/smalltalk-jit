"A thin LIFO facade over OrderedCollection. The top of the stack is the last
 element, so push:/pop/top map onto addLast:/removeLast/last."

Stack := OrderedCollection [

	push: anObject [
		^self addLast: anObject
	]


	pop [
		^self removeLast
	]


	top [
		^self last
	]

]
