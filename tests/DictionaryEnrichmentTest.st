"DictionaryEnrichmentTest: the wave 2 Dictionary protocol (at:ifAbsentPut:,
 at:ifPresent:ifAbsent:, associations, valuesDo:, keysSortedSafely,
 associationsSelect:, collectValues:) and the Bag counting protocol
 (valuesAndCounts, sortedCounts). Confirms at:ifAbsentPut: runs its block at
 most once and that the association selectors preserve keys."

[
	| t d count present absent selected mapped b vc sc |
	t := TestRun named: 'DictionaryEnrichment'.

	"---- at:ifAbsentPut: evaluates the block exactly once ----"
	d := Dictionary new.
	count := 0.
	t check: 'at:ifAbsentPut: returns the stored value' equals: 10 is: [
		d at: #a ifAbsentPut: [count := count + 1. 10] ].
	t check: 'at:ifAbsentPut: does not overwrite' equals: 10 is: [
		d at: #a ifAbsentPut: [count := count + 1. 20] ].
	t check: 'at:ifAbsentPut: block ran exactly once' equals: 1 is: [ count ].
	t check: 'at:ifAbsentPut: stored the key' equals: 10 is: [ d at: #a ].

	"---- at:ifPresent:ifAbsent: ----"
	d := Dictionary new. d at: #a put: 10.
	present := d at: #a ifPresent: [:v | v + 1] ifAbsent: [-1].
	absent := d at: #z ifPresent: [:v | v] ifAbsent: [-1].
	t check: 'at:ifPresent:ifAbsent: present branch' equals: 11 is: [ present ].
	t check: 'at:ifPresent:ifAbsent: absent branch' equals: -1 is: [ absent ].

	"---- associations / valuesDo: ----"
	d := Dictionary new. d at: 1 put: 10; at: 2 put: 20; at: 3 put: 30.
	t check: 'associations size' equals: 3 is: [ d associations size ].
	t check: 'associations are Associations' that: [ d associations allSatisfy: [:a | a isKindOf: Association] ].
	t check: 'valuesDo: sums values' equals: 60 is: [
		count := 0. d valuesDo: [:v | count := count + v]. count ].

	"---- keysSortedSafely ----"
	d := Dictionary new. d at: 3 put: #c; at: 1 put: #a; at: 2 put: #b.
	t check: 'keysSortedSafely ascending' equals: #(1 2 3) is: [ d keysSortedSafely ].

	"---- associationsSelect: preserves the surviving key/value pairs ----"
	d := Dictionary new. d at: 1 put: 1; at: 2 put: 2; at: 3 put: 3; at: 4 put: 4.
	selected := d associationsSelect: [:assoc | assoc value even].
	t check: 'associationsSelect: answers a Dictionary' that: [ selected isKindOf: Dictionary ].
	t check: 'associationsSelect: keeps matching size' equals: 2 is: [ selected size ].
	t check: 'associationsSelect: keeps the value at its key' equals: 2 is: [ selected at: 2 ].
	t check: 'associationsSelect: drops non-matching keys' that: [ (selected includesKey: 1) not ].

	"---- collectValues: keeps the keys, transforms the values ----"
	d := Dictionary new. d at: 1 put: 1; at: 2 put: 2; at: 3 put: 3.
	mapped := d collectValues: [:v | v * 10].
	t check: 'collectValues: answers a Dictionary' that: [ mapped isKindOf: Dictionary ].
	t check: 'collectValues: keeps the keys' equals: #(1 2 3) is: [ mapped keysSortedSafely ].
	t check: 'collectValues: transforms a value' equals: 20 is: [ mapped at: 2 ].
	t check: 'collectValues: same size' equals: 3 is: [ mapped size ].

	"---- Bag valuesAndCounts ----"
	b := Bag new. b add: #a; add: #a; add: #b.
	vc := b valuesAndCounts.
	t check: 'valuesAndCounts is a Dictionary' that: [ vc isKindOf: Dictionary ].
	t check: 'valuesAndCounts counts a' equals: 2 is: [ vc at: #a ].
	t check: 'valuesAndCounts counts b' equals: 1 is: [ vc at: #b ].

	"---- Bag sortedCounts (descending by count) ----"
	b := Bag new. b add: #a withOccurrences: 3; add: #b withOccurrences: 1; add: #c withOccurrences: 2.
	sc := b sortedCounts.
	t check: 'sortedCounts is an OrderedCollection' that: [ sc isKindOf: OrderedCollection ].
	t check: 'sortedCounts descending counts' equals: #(3 2 1) is: [ (sc collect: [:a | a key]) asArray ].
	t check: 'sortedCounts top element' equals: #a is: [ (sc first) value ].
	t check: 'sortedCounts pairs count -> element' equals: #c is: [ (sc at: 2) value ].

	^t report
]
