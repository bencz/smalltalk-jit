"Tier-1 speculation invalidation when the guarded class is True or False.

 This is the same contract as TierInlineRedefineTest, but for the one class of
 receiver whose class check has a FAST PATH in generateClassCheck. `true` and
 `false` are singletons, so an ordinary boolean check compiles to a pointer
 compare against the one instance instead of the generic four-way tag dispatch
 plus class compare. That fast path needs two compares and jumps forward over
 its own tail, so it does NOT end in the single fixed-width conditional branch
 that jit/SpecSite.h relies on.

 That matters because the site registration subtracts a FIXED branch width to
 find the branch it just emitted. With the boolean fast path in place it lands
 in the middle of an instruction, and targetPoisonGuardBranch would rewrite
 those bytes: code corruption, not a missed optimisation. Verified by allowing
 the fast path for speculations on purpose and running this file, which trips
 `site[0] == 0x0F && (site[1] & 0xF0) == 0x80` in vm/jit/x64/Abi.c. That
 assertion is load-bearing rather than decorative here, and it survives release
 builds: CMakeLists.txt sets CMAKE_C_FLAGS_RELEASE to plain -O2 and never
 defines NDEBUG, so this fails loudly in every configuration this project
 ships. Both backends refuse the fast path for speculation guards, and this
 file is what proves the refusal is still in place.

 Measured before writing it: a speculation guard whose class is True or False
 occurs 3 times across tests/, samples/ and benchmarks/, so the combination is
 reachable and was simply never invalidated by any test.

 WARNING on test shape, inherited from TierInlineRedefineTest: the tier counter
 is decremented in the METHOD PROLOGUE only and there is no OSR, so a loop
 inside one call never reaches tier 1. The driver must be CALLED repeatedly."

True extend [ tierBoolProbe [ ^#originalTrue ] ]
False extend [ tierBoolProbe [ ^#originalFalse ] ]

TierBoolDriver := Object [

	| flag |

	setFlag: aBoolean [ flag := aBoolean. ^self ]

	"One send per call, receiver monomorphic on a boolean, so repeated CALLS
	 heat the tier counter and the site inlines behind a True (or False)
	 exact-class guard."
	call [ ^flag tierBoolProbe ]

	class redefineTrueTo: aSymbolSource [
		"removeSelector: then extend, because `extend` alone refuses to redefine
		 a selector the class already has. Both halves invalidate send caches,
		 which is exactly the path the speculation poison hangs off."
		True removeSelector: #tierBoolProbe.
		Compiler new buildClass:
			(Parser parseString: 'True extend [ tierBoolProbe [ ^',
				aSymbolSource, ' ] ]') parseClass
	]

]

[ | t d f sum |
	t := TestRun named: 'TierBooleanGuard'.

	"--- True receiver ---"
	d := TierBoolDriver new setFlag: true.
	1 to: 20000 do: [:i | d call].
	t check: 'warm site on a True receiver answers the original'
		equals: #originalTrue is: [ d call ].

	"--- False receiver, its own warm site ---"
	f := TierBoolDriver new setFlag: false.
	1 to: 20000 do: [:i | f call].
	t check: 'a False receiver answers its own method'
		equals: #originalFalse is: [ f call ].

	"--- the same warm site must handle the OTHER boolean, which misses the
	     exact-class guard and has to take the fallback send ---"
	d setFlag: false.
	t check: 'the warm True site falls back correctly for a False receiver'
		equals: #originalFalse is: [ d call ].
	d setFlag: true.
	t check: 'and back again for a True receiver'
		equals: #originalTrue is: [ d call ].

	"--- redefinition: the poison has to reach the inlined boolean guard ---"
	TierBoolDriver redefineTrueTo: '#redefinedTrue'.

	t check: 'a cold direct send sees the redefinition'
		equals: #redefinedTrue is: [ true tierBoolProbe ].
	t check: 'the WARM inlined boolean site sees the redefinition'
		equals: #redefinedTrue is: [ d call ].

	sum := 0.
	1 to: 100 do: [:i | (d call == #redefinedTrue) ifFalse: [ sum := sum + 1 ] ].
	t check: '100 further calls all see the redefinition' equals: 0 is: [ sum ].

	"--- a second redefinition, so the fix cannot be a one-shot ---"
	TierBoolDriver redefineTrueTo: '#redefinedAgain'.
	t check: 'the warm boolean site tracks a SECOND redefinition'
		equals: #redefinedAgain is: [ d call ].

	"--- the False side must be untouched by all of that ---"
	t check: 'the False receiver still answers its own, unredefined method'
		equals: #originalFalse is: [ f call ].

	^t report
]
