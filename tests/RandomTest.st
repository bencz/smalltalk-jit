"RandomTest: the Park-Miller MINSTD generator. A fixed seed reproduces its
 sequence and different seeds diverge; next lands in [0, 1); nextInteger: n
 covers 1..n with every endpoint reached; between:and: is inclusive; and the
 collection helpers built on Random default (shuffled, atRandom) preserve or
 belong to their source."

[
	| t r1 r2 s1 s2 seen draws lo hi allIn sawLo sawHi f orig sh member i |
	t := TestRun named: 'Random'.

	"---- a fixed seed reproduces the sequence ----"
	r1 := Random seed: 42.
	r2 := Random seed: 42.
	s1 := (1 to: 30) collect: [:x | r1 nextValue].
	s2 := (1 to: 30) collect: [:x | r2 nextValue].
	t check: 'same seed reproduces the sequence' that: [ s1 = s2 ].

	"---- different seeds diverge ----"
	t check: 'different seeds diverge' that: [
		(Random seed: 1) nextValue ~= (Random seed: 2) nextValue ].

	"---- seed 0 is normalised (no fixed point) ----"
	t check: 'seed 0 does not stick at 0' that: [ (Random seed: 0) nextValue ~= 0 ].

	"---- next lands in [0, 1) ----"
	r1 := Random seed: 7.
	allIn := true.
	sawLo := false.
	1000 timesRepeat: [
		f := r1 next.
		(f >= 0 and: [f < 1]) ifFalse: [allIn := false].
		f < 0.5 ifTrue: [sawLo := true]].
	t check: 'next is a Float' that: [ (Random seed: 1) next isKindOf: Float ].
	t check: 'next stays within [0,1)' that: [ allIn ].
	t check: 'next reaches the lower half' that: [ sawLo ].

	"---- nextInteger: n covers 1..n, all endpoints reached ----"
	r1 := Random seed: 99.
	seen := Set new.
	allIn := true.
	1000 timesRepeat: [
		draws := r1 nextInteger: 5.
		(draws >= 1 and: [draws <= 5]) ifFalse: [allIn := false].
		seen add: draws].
	t check: 'nextInteger: stays within 1..n' that: [ allIn ].
	t check: 'nextInteger: reaches all of 1..5' equals: 5 is: [ seen size ].
	t check: 'nextInteger: includes 1' that: [ seen includes: 1 ].
	t check: 'nextInteger: includes 5' that: [ seen includes: 5 ].
	t expect: 'nextInteger: 0 raises' raises: Error in: [ (Random seed: 1) nextInteger: 0 ].

	"---- between:and: is inclusive of both ends ----"
	r1 := Random seed: 123.
	lo := 3. hi := 7.
	allIn := true. sawLo := false. sawHi := false.
	1000 timesRepeat: [
		draws := r1 between: lo and: hi.
		(draws >= lo and: [draws <= hi]) ifFalse: [allIn := false].
		draws = lo ifTrue: [sawLo := true].
		draws = hi ifTrue: [sawHi := true]].
	t check: 'between:and: stays in range' that: [ allIn ].
	t check: 'between:and: reaches the low end' that: [ sawLo ].
	t check: 'between:and: reaches the high end' that: [ sawHi ].

	"---- nextBoolean produces both values ----"
	r1 := Random seed: 555.
	sawLo := false. sawHi := false.
	100 timesRepeat: [ (r1 nextBoolean) ifTrue: [sawHi := true] ifFalse: [sawLo := true] ].
	t check: 'nextBoolean yields both true and false' that: [ sawLo and: [sawHi] ].

	"---- shuffled preserves the elements (as a multiset) ----"
	orig := #(3 1 4 1 5 9 2 6 5 3).
	sh := orig shuffled.
	t check: 'shuffled answers an Array' that: [ sh isKindOf: Array ].
	t check: 'shuffled keeps the same size' equals: 10 is: [ sh size ].
	t check: 'shuffled preserves every element' that: [
		sh asSortedCollection asArray = orig asSortedCollection asArray ].
	t check: 'shuffled does not mutate the source' equals: #(3 1 4 1 5 9 2 6 5 3) is: [ orig ].

	"---- atRandom belongs to the receiver ----"
	member := true.
	i := 0.
	[ i < 50 ] whileTrue: [
		(#(10 20 30) includes: (#(10 20 30) atRandom)) ifFalse: [member := false].
		i := i + 1].
	t check: 'atRandom returns a member' that: [ member ].

	^t report
]
