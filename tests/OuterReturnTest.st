OuterReturnTest := Object [

	class createBlock [
		^[^1]
	]


	class outerReturn [
		[^1].
		[[^4] value. ^2] value.
		^3
	]

	class outerReturn2 [
		true ifTrue: [^1].
		^2
	]

	class outerReturn3 [
		true ifFalse: [^1].
		^2
	]
]


[
	OuterRun := TestRun named: 'OuterReturn'.

	OuterRun expect: 'escaped block outer return raises' raises: Error in: [OuterReturnTest createBlock value].
	OuterRun check: 'nested outer return' equals: 4 is: [OuterReturnTest outerReturn].
	OuterRun check: 'ifTrue outer return' equals: 1 is: [OuterReturnTest outerReturn2].
	OuterRun check: 'ifFalse skips outer return' equals: 2 is: [OuterReturnTest outerReturn3].
]


[ ^OuterRun report ]
