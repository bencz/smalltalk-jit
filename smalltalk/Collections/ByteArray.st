ByteArray := ArrayedCollection [

	<shape: BytesShape>

	at: index put: aByte [
		"A ByteArray stores unsigned bytes. Reject anything outside 0..255 with a
		 clear OutOfRangeError instead of silently masking to the low 8 bits (the
		 raw AtPutPrimitive truncates). The index bounds check is left to super."
		((aByte isKindOf: SmallInteger) and: [aByte >= 0 and: [aByte <= 255]])
			ifFalse: [^(OutOfRangeError value: aByte between: 0 and: 255) signal].
		^super at: index put: aByte
	]

]
