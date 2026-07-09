"Real multi-operation floating-point computations that exercise the JIT Float
 intrinsic (inlined + - * / and comparisons) together with the trig/sqrt
 primitives and coercion: great-circle geo-distance (Haversine), 3D Euclidean
 distance, quadratic roots, compound interest, mean/standard-deviation, and a
 pi series. Returns ^failCount (0 = pass) so run_tests.sh catches a regression
 (an Assert failure only prints and still exits 0 here — see BooleanGlobalTest.st).
 Tolerances (`close:to:within:`) absorb IEEE rounding; the geo checks lean on
 self-consistent anchors (a point to itself is 0 km, dist is symmetric, 1 degree
 on the equator is R*pi/180) plus a couple of known city pairs."

GeoMath := Object [
	"Great-circle distance in kilometres between two lat/lon points (Haversine).
	 R is the mean Earth radius; angles come in degrees."
	kmFrom: lat1 lon: lon1 to: lat2 lon: lon2 [
		| r p1 p2 dLat dLon a |
		r := 6371.0088.
		p1 := lat1 degreesToRadians.
		p2 := lat2 degreesToRadians.
		dLat := (lat2 - lat1) degreesToRadians.
		dLon := (lon2 - lon1) degreesToRadians.
		a := (dLat / 2.0) sin squared + (p1 cos * p2 cos * (dLon / 2.0) sin squared).
		^r * 2.0 * a sqrt arcSin
	]

	distanceX: x1 y: y1 z: z1 toX: x2 y: y2 z: z2 [
		^((x2 - x1) squared + (y2 - y1) squared + (z2 - z1) squared) sqrt
	]

	"Real roots of a*x^2 + b*x + c = 0 via the quadratic formula (larger root first)."
	quadraticA: a b: b c: c [
		| disc coll |
		disc := (b squared - (4.0 * a * c)) sqrt.
		coll := OrderedCollection new.
		coll add: (b negated + disc) / (2.0 * a).
		coll add: (b negated - disc) / (2.0 * a).
		^coll
	]

	compound: principal rate: r years: n [
		^principal * ((1.0 + r) raisedTo: n)
	]

	mean: coll [
		| s |
		s := 0.0.
		coll do: [:x | s := s + x].
		^s / coll size
	]

	stddev: coll [
		| m s |
		m := self mean: coll.
		s := 0.0.
		coll do: [:x | s := s + (x - m) squared].
		^(s / coll size) sqrt
	]

	"pi via the Nilakantha series: 3 + 4/(2*3*4) - 4/(4*5*6) + ..."
	piWithTerms: terms [
		| sum sign k |
		sum := 3.0. sign := 1.0. k := 1.
		[k <= terms] whileTrue: [
			| d |
			d := (2.0 * k) * ((2.0 * k) + 1.0) * ((2.0 * k) + 2.0).
			sum := sum + (sign * (4.0 / d)).
			sign := sign negated.
			k := k + 1].
		^sum
	]

	close: a to: b within: t [ ^(a - b) abs < t ]

	"--- non-geo numeric checks (each returns 0 on pass, 1 on failure) ---"
	checkMath [
		| fails data roots |
		fails := 0.

		"3D Euclidean distance: (0,0,0)->(3,4,12) is exactly 13"
		(self close: (self distanceX: 0.0 y: 0.0 z: 0.0 toX: 3.0 y: 4.0 z: 12.0) to: 13.0 within: 0.0000001)
			ifFalse: [ fails := fails + 1 ].

		"x^2 - 5x + 6 = 0  =>  roots 3 and 2"
		roots := self quadraticA: 1.0 b: -5.0 c: 6.0.
		(self close: (roots at: 1) to: 3.0 within: 0.0000001) ifFalse: [ fails := fails + 1 ].
		(self close: (roots at: 2) to: 2.0 within: 0.0000001) ifFalse: [ fails := fails + 1 ].

		"compound interest: 1000 at 5% for 10 years"
		(self close: (self compound: 1000.0 rate: 0.05 years: 10) to: 1628.894626777442 within: 0.001)
			ifFalse: [ fails := fails + 1 ].

		"mean = 5, population stddev = 2 for this dataset"
		data := OrderedCollection new.
		#(2 4 4 4 5 5 7 9) do: [:x | data add: x asFloat].
		(self close: (self mean: data) to: 5.0 within: 0.0000001) ifFalse: [ fails := fails + 1 ].
		(self close: (self stddev: data) to: 2.0 within: 0.0000001) ifFalse: [ fails := fails + 1 ].

		"pi series converges to Float pi"
		(self close: (self piWithTerms: 500) to: Float pi within: 0.0001) ifFalse: [ fails := fails + 1 ].
		^fails
	]

	"--- great-circle geo-distance checks ---"
	checkGeo [
		| fails lp nyla |
		fails := 0.

		"a point to itself is 0 km"
		(self close: (self kmFrom: 40.0 lon: 10.0 to: 40.0 lon: 10.0) to: 0.0 within: 0.001)
			ifFalse: [ fails := fails + 1 ].

		"distance is symmetric"
		(self close: (self kmFrom: 51.5 lon: 0.0 to: 48.85 lon: 2.35)
			to: (self kmFrom: 48.85 lon: 2.35 to: 51.5 lon: 0.0) within: 0.001)
			ifFalse: [ fails := fails + 1 ].

		"one degree along the equator == R * pi/180 ~ 111.19 km"
		(self close: (self kmFrom: 0.0 lon: 0.0 to: 0.0 lon: 1.0) to: 111.195 within: 0.1)
			ifFalse: [ fails := fails + 1 ].

		"London -> Paris ~ 344 km"
		lp := self kmFrom: 51.5074 lon: -0.1278 to: 48.8566 lon: 2.3522.
		((lp > 342.0) and: [ lp < 346.0 ]) ifFalse: [ fails := fails + 1 ].

		"New York -> Los Angeles ~ 3936 km"
		nyla := self kmFrom: 40.7128 lon: -74.0060 to: 34.0522 lon: -118.2437.
		((nyla > 3920.0) and: [ nyla < 3950.0 ]) ifFalse: [ fails := fails + 1 ].
		^fails
	]
]

[
	| g fails |
	g := GeoMath new.
	fails := g checkMath + g checkGeo.
	fails > 0 ifTrue: [
		Transcript nextPutAll: 'FloatMathTest FAILURES='; nextPutAll: fails printString; lf ].
	^fails
]
