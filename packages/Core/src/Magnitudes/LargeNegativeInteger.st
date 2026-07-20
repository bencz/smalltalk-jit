"Arbitrary precision integer smaller than the SmallInteger minimum. The sign
of a LargeInteger lives in its class, Pharo style; the whole protocol is
inherited from LargeInteger, which asks `self negative` wherever the sign
matters."

LargeNegativeInteger := LargeInteger [

	negative [
		^true
	]

]
