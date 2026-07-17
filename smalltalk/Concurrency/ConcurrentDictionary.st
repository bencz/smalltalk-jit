"A read-mostly concurrent dictionary with LOCK-FREE reads.

 Reads (at:, at:ifAbsent:, includesKey:, size, do:, ...) run WITHOUT the global
 sync monitor: they load the current immutable snapshot (one ivar read) and probe
 it. Many workers can look up in parallel with zero lock traffic, which is the
 whole point: the actor registry and id->pid directories are read on every message
 but written only on spawn/death, so serializing every lookup on the one global
 monitor (as SharedDictionary does) is the multicore ceiling.

 Writes are copy-on-write under the monitor: build a fresh inner Dictionary that
 shares no mutable structure with the old one (new contents Array, new
 Associations; keys and values are shared, which is intended), apply the change,
 and publish it with a single ivar store. A reader keeps using whatever snapshot
 it already loaded, which never mutates. A superseded snapshot becomes garbage as
 soon as no reader still holds it, and the GC reclaims it: the collector IS the
 reclamation grace period, so no hazard pointers or epochs are needed.

 Memory model: correct on x86-64 (TSO). The writer fully builds the snapshot
 before storing the pointer, so a reader that observes the new pointer also
 observes a fully-built snapshot, and the reader's slot loads are address-dependent
 on the pointer load. On a weakly-ordered target (POWER) the publish would need a
 release fence before the ivar store; the VM's multi-worker scheduler runs on
 x86-64, so that is documented rather than emitted here.

 Cost model: writes are O(n) (they rebuild the inner dictionary), so this is for
 READ-MOSTLY maps only. For write-heavy shared state use SharedDictionary."
ConcurrentDictionary := Object [

	| snapshot |


	class new [
		^super new initConcurrentDictionary: 31
	]


	class new: anInteger [
		^super new initConcurrentDictionary: anInteger
	]


	initConcurrentDictionary: anInteger [
		snapshot := Dictionary new: anInteger.
		^self
	]


	"reads -- LOCK-FREE: capture the immutable snapshot once, then probe it. The
	 send evaluates `snapshot` to the receiver before dispatching, so the whole
	 lookup runs against one consistent, never-mutated Dictionary even if a writer
	 publishes a new snapshot meanwhile."

	at: key ifAbsent: aBlock [
		^snapshot at: key ifAbsent: aBlock
	]


	at: key [
		^snapshot at: key ifAbsent: [ (NotFoundError value: key) signal ]
	]


	at: key ifPresent: aBlock [
		| v |
		v := snapshot at: key ifAbsent: [ ^nil ].
		^aBlock value: v
	]


	includesKey: key [
		^snapshot includesKey: key
	]


	size [
		^snapshot size
	]


	isEmpty [ ^snapshot isEmpty ]

	notEmpty [ ^snapshot notEmpty ]


	"writes -- copy-on-write under the monitor, publish a fresh snapshot"

	at: key put: anObject [
		| copy |
		Processor monitorEnter.
		copy := self copyOfSnapshot.
		copy at: key put: anObject.
		snapshot := copy.
		Processor monitorExit.
		^anObject
	]


	at: key ifAbsentPut: aBlock [
		| v |
		Processor monitorEnter.
		v := snapshot at: key ifAbsent: [ nil ].
		v isNil ifTrue: [
			| copy |
			v := aBlock value.
			copy := self copyOfSnapshot.
			copy at: key put: v.
			snapshot := copy ].
		Processor monitorExit.
		^v
	]


	removeKey: key ifAbsent: aBlock [
		| copy v had |
		Processor monitorEnter.
		had := snapshot includesKey: key.
		had ifTrue: [
			copy := self copyOfSnapshot.
			v := copy removeKey: key ifAbsent: [ nil ].
			snapshot := copy ].
		Processor monitorExit.
		had ifFalse: [ ^aBlock value ].
		^v
	]


	removeKey: key [
		^self removeKey: key ifAbsent: [ (NotFoundError value: key) signal ]
	]


	"A fresh Dictionary sharing NO mutable structure with the current snapshot:
	 re-adding through at:put: allocates a new contents Array and new Associations,
	 so mutating the copy can never touch a snapshot a reader is probing. CALLER
	 HOLDS THE MONITOR."
	copyOfSnapshot [
		| copy |
		copy := Dictionary new: snapshot size + 1.
		snapshot keysAndValuesDo: [ :k :v | copy at: k put: v ].
		^copy
	]


	"enumeration -- the snapshot is immutable, so iterate it directly, lock-free"

	do: aBlock [ snapshot do: aBlock ]

	keysDo: aBlock [ snapshot keysDo: aBlock ]

	keysAndValuesDo: aBlock [ snapshot keysAndValuesDo: aBlock ]

	associationsDo: aBlock [ snapshot associationsDo: aBlock ]

	keys [ ^snapshot keys ]

	values [ ^snapshot values ]


	printOn: aStream [
		aStream nextPutAll: 'ConcurrentDictionary'.
		snapshot printOn: aStream.
	]

]
