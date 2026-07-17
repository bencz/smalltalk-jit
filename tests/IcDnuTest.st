"doesNotUnderstand through inline-cached send sites, in BOTH warm-up orders.
 The lookup caches a DNU thunk like any other entry, so an IC can legitimately
 bind (class, dnu-thunk): a repeat send from the same class then dispatches
 STRAIGHT to the thunk. Order 1 binds the understood class first and hits DNU
 second; order 2 (a separate, fresh site) binds the DNU thunk FIRST and only
 then sees the understood class. Each send asserts either the answer or the
 raised MessageNotUnderstood."

IcDnuPresent := Object [ zork [ ^'present' ] ]
IcDnuMissing := Object [ ]

IcDnuSiteA := Object [
	class poke: x [ ^x zork ]
]
IcDnuSiteB := Object [
	class poke: x [ ^x zork ]
]

[ | fails expectAnswer expectDnu present missing |
	fails := 0.
	present := IcDnuPresent new.
	missing := IcDnuMissing new.

	expectAnswer := [:aBlock |
		([aBlock value] on: MessageNotUnderstood do: [:e | nil]) = 'present'
			ifFalse: [fails := fails + 1]].
	expectDnu := [:aBlock |
		([aBlock value. false] on: MessageNotUnderstood do: [:e | true])
			ifFalse: [fails := fails + 1]].

	"Order 1: understood first (site A binds IcDnuPresent), then DNU, then
	 interleaved: the bound class must keep answering while the other raises."
	1 to: 300 do: [:i | expectAnswer value: [IcDnuSiteA poke: present]].
	1 to: 300 do: [:i | expectDnu value: [IcDnuSiteA poke: missing]].
	1 to: 300 do: [:i |
		expectAnswer value: [IcDnuSiteA poke: present].
		expectDnu value: [IcDnuSiteA poke: missing]].

	"Order 2: DNU first (site B binds the DNU thunk for IcDnuMissing), repeat
	 sends hit that cached thunk, then the understood class must still work."
	1 to: 300 do: [:i | expectDnu value: [IcDnuSiteB poke: missing]].
	1 to: 300 do: [:i | expectAnswer value: [IcDnuSiteB poke: present]].
	1 to: 300 do: [:i |
		expectDnu value: [IcDnuSiteB poke: missing].
		expectAnswer value: [IcDnuSiteB poke: present]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'IcDnuTest: OK'; lf].
	^fails
]
