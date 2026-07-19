"A length of time held as an integer number of milliseconds (which may be
 negative). Durations add and subtract with each other and scale by a Number;
 the component accessors (days/hours/minutes/seconds/milliseconds) break the
 magnitude down like a clock, while the as* accessors give the total measured
 in one unit (possibly an exact Fraction). printOn: emits ISO 8601 duration
 syntax, e.g. P2DT3H4M5S, PT0S for zero, with a leading minus for negatives."

Duration := Magnitude [

	| millis |


	"--- instance creation ---"

	class milliseconds: n [
		^self new setMilliseconds: n
	]


	class seconds: n [
		^self new setMilliseconds: n * 1000
	]


	class minutes: n [
		^self new setMilliseconds: n * 60000
	]


	class hours: n [
		^self new setMilliseconds: n * 3600000
	]


	class days: n [
		^self new setMilliseconds: n * 86400000
	]


	class weeks: n [
		^self new setMilliseconds: n * 604800000
	]


	class days: d hours: h minutes: m seconds: s [
		^self new setMilliseconds:
			(d * 86400000) + (h * 3600000) + (m * 60000) + (s * 1000)
	]


	class zero [
		^self new setMilliseconds: 0
	]


	setMilliseconds: n [
		millis := n.
		^self
	]


	"--- component accessors (clock breakdown of the magnitude) ---"

	days [
		^millis abs // 86400000
	]


	hours [
		^(millis abs // 3600000) \\ 24
	]


	minutes [
		^(millis abs // 60000) \\ 60
	]


	seconds [
		^(millis abs // 1000) \\ 60
	]


	milliseconds [
		^millis abs \\ 1000
	]


	"--- totals ---"

	asMilliseconds [
		^millis
	]


	asSeconds [
		^millis / 1000
	]


	asMinutes [
		^millis / 60000
	]


	asHours [
		^millis / 3600000
	]


	asDays [
		^millis / 86400000
	]


	"--- arithmetic ---"

	+ aDuration [
		^Duration milliseconds: millis + aDuration asMilliseconds
	]


	- aDuration [
		^Duration milliseconds: millis - aDuration asMilliseconds
	]


	* aNumber [
		^Duration milliseconds: (millis * aNumber) rounded
	]


	/ aNumber [
		^Duration milliseconds: (millis / aNumber) rounded
	]


	negated [
		^Duration milliseconds: millis negated
	]


	abs [
		^Duration milliseconds: millis abs
	]


	"--- testing ---"

	isZero [
		^millis = 0
	]


	negative [
		^millis < 0
	]


	positive [
		^millis > 0
	]


	"--- comparing ---"

	< aDuration [
		^millis < aDuration asMilliseconds
	]


	= aDuration [
		^(aDuration isKindOf: Duration) and: [millis = aDuration asMilliseconds]
	]


	hash [
		^millis hash
	]


	"--- printing ---"

	printOn: aStream [
		| v d h m s ms |

		millis = 0 ifTrue: [aStream nextPutAll: 'PT0S'. ^self].
		millis < 0 ifTrue: [aStream nextPut: $-].
		v := millis abs.
		d := v // 86400000.
		h := (v // 3600000) \\ 24.
		m := (v // 60000) \\ 60.
		s := (v // 1000) \\ 60.
		ms := v \\ 1000.
		aStream nextPut: $P.
		d > 0 ifTrue: [d printOn: aStream. aStream nextPut: $D].
		(h > 0 or: [(m > 0) or: [(s > 0) or: [ms > 0]]]) ifTrue: [
			aStream nextPut: $T.
			h > 0 ifTrue: [h printOn: aStream. aStream nextPut: $H].
			m > 0 ifTrue: [m printOn: aStream. aStream nextPut: $M].
			(s > 0 or: [ms > 0]) ifTrue: [
				s printOn: aStream.
				ms > 0 ifTrue: [
					aStream nextPut: $..
					ms < 100 ifTrue: [aStream nextPut: $0].
					ms < 10 ifTrue: [aStream nextPut: $0].
					ms printOn: aStream].
				aStream nextPut: $S]].
	]

]
