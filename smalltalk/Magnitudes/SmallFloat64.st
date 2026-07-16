"Immediate double precision float. The value rides in the tagged word itself
(low tag bits 0b11), so instances have no heap object and no header: identity
is bit equality, like SmallInteger. identityHash therefore has to be derived
from the value; the header-reading HashPrimitive would dereference garbage."

SmallFloat64 := Float [

	identityHash [
		^self hash
	]

]
