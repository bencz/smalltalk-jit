"Heap allocated double precision float, used for values outside the immediate
SmallFloat64 range (subnormals, infinities, NaN and extreme magnitudes). The
IEEE-754 double is stored in the object's (unscanned) payload word."

BoxedFloat64 := Float [

	<shape: FloatShape>

]
