"A thread-safe Dictionary for the multi-worker scheduler. Every operation runs under the
 global sync monitor, so concurrent at:put:/at:ifAbsent:/removeKey: from fibers on
 different workers cannot corrupt the shared open-addressed hash (a plain Dictionary is
 NOT safe once the scheduler runs fibers on several OS threads by default).

 User blocks (ifAbsent:, ifPresent:, do:, keysAndValuesDo:, ...) are evaluated OUTSIDE the
 monitor — on a snapshot when iterating — so they can freely touch other synchronized
 state without deadlocking the non-reentrant monitor. A missing key is told apart from a
 stored nil by a private sentinel, so nil is a valid value.

 Coarse-grained: one global monitor serializes all shared-collection access. Correct but
 not maximally scalable; per-object / striped locking is a performance follow-up."
SharedDictionary := Object [

	| dict absent |


	class new [
		^super new initSharedDictionary
	]


	class withAll: aDictionary [
		| d |
		d := self new.
		aDictionary keysAndValuesDo: [:k :v | d at: k put: v ].
		^d
	]


	initSharedDictionary [
		dict := Dictionary new.
		absent := Object new.  "unique per-instance sentinel: distinguishes absent from a stored nil"
		^self
	]


	"accessing"

	at: key put: anObject [
		Processor monitorEnter.
		dict at: key put: anObject.
		Processor monitorExit.
		^anObject
	]


	at: key ifAbsent: aBlock [
		| v |
		Processor monitorEnter.
		v := dict at: key ifAbsent: [ absent ].
		Processor monitorExit.
		v == absent ifTrue: [ ^aBlock value ].
		^v
	]


	at: key [
		^self at: key ifAbsent: [ (NotFoundError value: key) signal ]
	]


	at: key ifPresent: aBlock [
		| v |
		Processor monitorEnter.
		v := dict at: key ifAbsent: [ absent ].
		Processor monitorExit.
		v == absent ifTrue: [ ^nil ].
		^aBlock value: v
	]


	at: key ifAbsentPut: aBlock [
		"Atomically return the value for key, computing + storing aBlock's value if absent.
		 aBlock is evaluated under the monitor, so it MUST NOT touch other synchronized state."
		| v |
		Processor monitorEnter.
		v := dict at: key ifAbsent: [ | nv | nv := aBlock value. dict at: key put: nv. nv ].
		Processor monitorExit.
		^v
	]


	"removing"

	removeKey: key ifAbsent: aBlock [
		| v |
		Processor monitorEnter.
		v := dict removeKey: key ifAbsent: [ absent ].
		Processor monitorExit.
		v == absent ifTrue: [ ^aBlock value ].
		^v
	]


	removeKey: key [
		^self removeKey: key ifAbsent: [ (NotFoundError value: key) signal ]
	]


	"testing"

	includesKey: key [
		| r |
		Processor monitorEnter.
		r := dict includesKey: key.
		Processor monitorExit.
		^r
	]


	size [
		| n |
		Processor monitorEnter.
		n := dict size.
		Processor monitorExit.
		^n
	]


	isEmpty [ ^self size = 0 ]

	notEmpty [ ^self size > 0 ]


	"enumerating (snapshot: the block runs outside the monitor)"

	keys [
		| k |
		Processor monitorEnter.
		k := dict keys.
		Processor monitorExit.
		^k
	]


	values [
		| v |
		Processor monitorEnter.
		v := dict values.
		Processor monitorExit.
		^v
	]


	do: aBlock [
		self values do: aBlock.
		^self
	]


	keysDo: aBlock [
		self keys do: aBlock.
		^self
	]


	keysAndValuesDo: aBlock [
		| snap |
		Processor monitorEnter.
		snap := OrderedCollection new.
		dict keysAndValuesDo: [:k :v | snap addLast: (k -> v) ].
		Processor monitorExit.
		snap do: [:assoc | aBlock value: assoc key value: assoc value ].
		^self
	]


	associationsDo: aBlock [
		| snap |
		Processor monitorEnter.
		snap := OrderedCollection new.
		dict associationsDo: [:assoc | snap addLast: assoc ].
		Processor monitorExit.
		snap do: aBlock.
		^self
	]


	printOn: aStream [
		Processor monitorEnter.
		dict printOn: aStream.
		Processor monitorExit.
	]

]
