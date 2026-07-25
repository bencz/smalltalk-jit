"FloatArray: unboxed IEEE-754 doubles in a BytesShape payload.

 The load-bearing invariant is the one the class comment warns about: basicSize
 answers BYTES (8 per element) and size answers ELEMENTS. Every inherited method
 either reasons in bytes consistently (shallowCopy, deepCopy and = pair
 basicSize with basicAt:/memcmp) or in elements consistently (do:, collect:,
 inject:into:, which SequenceableCollection defines over size and at:). A method
 that mixed the two would read eight times too many elements or an eighth of
 them, so the enumeration protocol is exercised here rather than assumed.

 Also covered: the full double range including the values that must BOX on read
 (outside the immediate SmallFloat64 window), NaN and both infinities, signed
 zero, bounds and type errors, the coercion of a non-Float Number on store, and
 the bulk kernels checked against the same computation done element by element
 in Smalltalk.

 Two round trips live outside this file because they cannot be reached from a
 test running in a loaded image: the SNAPSHOT round trip is the
 FLOATARRAY_SNAPSHOT check in run_tests.sh (build a project image, reload it,
 read the array back) and the ACTOR message round trip is case 7 of
 ST_MESSAGE_TEST (vm/runtime/Message.c), which memcmps the wire bytes."

[ | t a b n ref bad |
	t := TestRun named: 'FloatArray'.

	"--- shape: elements versus bytes ---"
	a := FloatArray new: 4.
	t check: 'size is the ELEMENT count' equals: 4 is: [ a size ].
	t check: 'basicSize is the BYTE count' equals: 32 is: [ a basicSize ].
	t check: 'a fresh array is all zero' equals: 0.0 is: [ a at: 1 ].
	t check: 'an empty array has size 0' equals: 0 is: [ (FloatArray new: 0) size ].
	t check: 'an empty array is empty' equals: true is: [ (FloatArray new: 0) isEmpty ].

	"--- accessing ---"
	a at: 1 put: 1.5; at: 2 put: -0.25; at: 3 put: 0.0; at: 4 put: 100.0.
	t check: 'stores and reads back' equals: 1.5 is: [ a at: 1 ].
	t check: 'negative fraction' equals: -0.25 is: [ a at: 2 ].
	t check: 'at:put: answers the stored value' equals: 7.5 is: [ a at: 1 put: 7.5 ].
	a at: 1 put: 1.5.
	t check: 'first is at:1' equals: 1.5 is: [ a first ].
	t check: 'last is at:size' equals: 100.0 is: [ a last ].

	"an integer stores as its double, and reads back as a Float"
	a at: 3 put: 42.
	t check: 'an integer argument is stored as a double' equals: 42.0 is: [ a at: 3 ].
	t check: 'and reads back as a Float, not an Integer' equals: true is: [ (a at: 3) isFloat ].

	"a Number that is neither Float nor SmallInteger is coerced, not rejected"
	a at: 3 put: (1/8).
	t check: 'a Fraction is coerced to the nearest double' equals: 0.125 is: [ a at: 3 ].
	a at: 3 put: (2 raisedTo: 70).
	t check: 'a LargeInteger is coerced too' equals: (2 raisedTo: 70) asFloat is: [ a at: 3 ].
	a at: 3 put: 0.0.

	"--- the whole double range, including values that must box on read ---"
	b := FloatArray new: 8.
	b at: 1 put: 1.0e300;      "outside the immediate window: boxes on read"
	  at: 2 put: 1.0e-300;
	  at: 3 put: Float infinity;
	  at: 4 put: Float infinity negated;
	  at: 5 put: Float nan;
	  at: 6 put: 0.0;
	  at: 7 put: 0.1;          "not exactly one tenth"
	  at: 8 put: 2.5.          "inside the window: stays immediate on read"
	t check: 'a huge value round-trips exactly' equals: 1.0e300 is: [ b at: 1 ].
	t check: 'and reads back BOXED' equals: BoxedFloat64 is: [ (b at: 1) class ].
	t check: 'a tiny value round-trips exactly' equals: 1.0e-300 is: [ b at: 2 ].
	t check: 'positive infinity survives' equals: true is: [ (b at: 3) isInfinite ].
	t check: 'and keeps its sign' equals: true is: [ (b at: 3) > 0 ].
	t check: 'negative infinity survives' equals: true is: [ (b at: 4) < 0 ].
	t check: 'NaN survives' equals: true is: [ (b at: 5) isNaN ].
	t check: 'NaN is not equal to itself, per IEEE' equals: false is: [ (b at: 5) = (b at: 5) ].
	t check: '0.1 round-trips bit-exactly' equals: 0.1 is: [ b at: 7 ].
	t check: 'and is still NOT one tenth' equals: false
		is: [ (b at: 7) asExactFraction = (1/10) ].
	t check: 'an in-range value reads back IMMEDIATE' equals: SmallFloat64
		is: [ (b at: 8) class ].

	"--- errors: a bad index and a bad value must both be clean, not silent ---"
	t expect: 'index 0 raises' raises: Error in: [ a at: 0 ].
	t expect: 'index past the end raises' raises: Error in: [ a at: 5 ].
	t expect: 'a negative index raises' raises: Error in: [ a at: -1 ].
	t expect: 'storing past the end raises' raises: Error in: [ a at: 5 put: 1.0 ].
	t expect: 'storing at 0 raises' raises: Error in: [ a at: 0 put: 1.0 ].
	t expect: 'storing a String raises' raises: Error in: [ a at: 1 put: 'x' ].
	t expect: 'storing nil raises' raises: Error in: [ a at: 1 put: nil ].
	t check: 'a failed store left the element alone' equals: 1.5 is: [ a at: 1 ].

	"--- enumeration: proves size/at: are element based all the way up ---"
	t check: 'do: visits each ELEMENT once' equals: 4
		is: [ | c | c := 0. a do: [:each | c := c + 1]. c ].
	"contents here are 1.5, -0.25, 0.0, 100.0"
	t check: 'inject:into: sees the values' equals: 101.25
		is: [ a inject: 0.0 into: [:acc :each | acc + each] ].
	t check: 'collect: sees the values' equals: 202.5
		is: [ (a collect: [:each | each * 2]) inject: 0.0 into: [:acc :e | acc + e] ].
	t check: 'detect: works' equals: 100.0 is: [ a detect: [:each | each > 50] ].
	t check: 'asArray has element count' equals: 4 is: [ a asArray size ].
	t check: 'asArray holds Floats' equals: 1.5 is: [ a asArray first ].
	t check: 'includes:' equals: true is: [ a includes: 100.0 ].
	t check: 'printString shows elements, not bytes' equals: true
		is: [ (a printString indexOf: $1) > 0 and: [a printString size < 60] ].

	"--- copying and equality, which go through the BYTE protocol ---"
	t check: 'copy is equal' equals: true is: [ a copy = a ].
	t check: 'copy is a distinct object' equals: false is: [ a copy == a ].
	t check: 'copy is independent' equals: 1.5
		is: [ | c | c := a copy. c at: 1 put: 99.0. a at: 1 ].
	t check: 'copy has the same element count' equals: 4 is: [ a copy size ].
	t check: 'arrays of different contents differ' equals: false
		is: [ | c | c := a copy. c at: 1 put: 99.0. c = a ].

	"--- withAll: ---"
	t check: 'withAll: an Array' equals: 3 is: [ (FloatArray withAll: #(1 2 3)) size ].
	t check: 'withAll: converts elements' equals: 2.0
		is: [ (FloatArray withAll: #(1 2 3)) at: 2 ].

	"--- bulk kernels, each against the same work done element by element ---"
	n := 50.
	a := FloatArray new: n.
	b := FloatArray new: n.
	1 to: n do: [:i | a at: i put: i / 4.0. b at: i put: (i * 3) / 8.0].

	ref := 0.0.
	1 to: n do: [:i | ref := ref + (a at: i)].
	t check: 'sum matches the element-by-element sum' equals: ref is: [ a sum ].

	ref := 0.0.
	1 to: n do: [:i | ref := ref + ((a at: i) * (b at: i))].
	t check: 'dot: matches the element-by-element dot product' equals: ref is: [ a dot: b ].

	t check: 'max' equals: n / 4.0 is: [ a max ].
	t check: 'min' equals: 0.25 is: [ a min ].

	bad := 0.
	1 to: n do: [:i |
		((a + b) at: i) = ((a at: i) + (b at: i)) ifFalse: [bad := bad + 1].
		((a - b) at: i) = ((a at: i) - (b at: i)) ifFalse: [bad := bad + 1].
		((a * b) at: i) = ((a at: i) * (b at: i)) ifFalse: [bad := bad + 1].
		((a / b) at: i) = ((a at: i) / (b at: i)) ifFalse: [bad := bad + 1]].
	t check: 'elementwise + - * / all match, over 50 elements'
		equals: 0 is: [ bad ].

	bad := 0.
	1 to: n do: [:i |
		((a * 3.0) at: i) = ((a at: i) * 3.0) ifFalse: [bad := bad + 1].
		((a + 3.0) at: i) = ((a at: i) + 3.0) ifFalse: [bad := bad + 1].
		((a * 3) at: i) = ((a at: i) * 3.0) ifFalse: [bad := bad + 1]].
	t check: 'scalar operations match, integer scalar included'
		equals: 0 is: [ bad ].

	t check: 'a bulk result is a FloatArray' equals: FloatArray is: [ (a + b) class ].
	t check: 'a bulk result has the same size' equals: n is: [ (a + b) size ].
	t check: 'the operands are unchanged' equals: 0.25 is: [ a at: 1 ].

	t check: 'atAllPut: fills every element' equals: 0
		is: [ | c k | c := FloatArray new: 10. c atAllPut: 2.5.
			k := 0. c do: [:each | each = 2.5 ifFalse: [k := k + 1]]. k ].
	t check: 'atAllPut: answers the receiver' equals: 10
		is: [ (FloatArray new: 10) atAllPut: 1.0; size ].

	"--- kernel edge cases ---"
	t check: 'sum of an empty array is zero' equals: 0 is: [ (FloatArray new: 0) sum ].
	t expect: 'max of an empty array raises' raises: Error in: [ (FloatArray new: 0) max ].
	t expect: 'min of an empty array raises' raises: Error in: [ (FloatArray new: 0) min ].
	t expect: 'mismatched sizes raise on +' raises: Error
		in: [ a + (FloatArray new: 3) ].
	t expect: 'mismatched sizes raise on dot:' raises: Error
		in: [ a dot: (FloatArray new: 3) ].
	t expect: 'dot: with a non-FloatArray raises' raises: Error in: [ a dot: #(1 2 3) ].
	t expect: 'an operand of the wrong type raises' raises: Error in: [ a + 'x' ].
	t check: 'division by zero follows IEEE, it does not raise' equals: true
		is: [ | c | c := FloatArray new: 1. c at: 1 put: 1.0.
			((c / 0.0) at: 1) isInfinite ].

	"--- a big array: the size where the representation actually matters ---"
	a := FloatArray new: 100000.
	a atAllPut: 0.5.
	t check: 'a 100k array sums exactly' equals: 50000.0 is: [ a sum ].
	t check: 'and its byte size is 8 per element' equals: 800000 is: [ a basicSize ].

	^t report
]
