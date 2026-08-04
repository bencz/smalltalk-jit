SourceCode := Object [

	| source position sourceSize line column |


	"accessing"

	source [
		^source
	]


	position [
		^position
	]


	sourceSize [
		^sourceSize
	]


	sourceContents [
		"THE TEXT THIS SOURCECODE NAMES, which is a RANGE of `source` and not
		 all of it: one SourceCode is made per method (and per block and per
		 class) over the same string, and answering the whole string made
		 `aMethod sourceNode` reparse the file from the top and describe
		 whatever came first in it.

		 Without a position or a size there is no range to take, which is what a
		 SourceCode built for a parse error looks like, and then the whole
		 string is the honest answer. FileSourceCode overrides this to re-read
		 the same range from the file instead."
		| start stop |

		(position isNil or: [ sourceSize isNil ]) ifTrue: [ ^source ].
		start := position.
		stop := position + sourceSize - 1.
		(start < 1 or: [ stop > source size ]) ifTrue: [ ^source ].
		^source copyFrom: start to: stop
	]


	line [
		^line
	]


	column [
		^column
	]


	"printing"

	printOn: aStream [
		aStream nextPut: $'; nextPutAll: source; nextPut: $'; nextPut: $:.
		line printOn: aStream.
		aStream nextPut: $:.
		column printOn: aStream.
	]

]
