Stats := Object [

	class sum: aCollection [
		^aCollection inject: 0 into: [ :a :b | a + b ]
	]

	class mean: aCollection [
		^(self sum: aCollection) / aCollection size
	]

]
