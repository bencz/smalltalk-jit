"Arbitrary precision integer greater than the SmallInteger maximum. The sign
of a LargeInteger lives in its class, Pharo style; the whole protocol is
inherited from LargeInteger, which asks `self negative` wherever the sign
matters."

LargePositiveInteger := LargeInteger [

	negative [
		^false
	]

]
