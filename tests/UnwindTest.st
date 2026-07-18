"UnwindTest: ensure:/ifCurtailed: must be REAL unwind-frame protection.

 Before the unwind rework, ensure:/ifCurtailed: were emulated as
 `on: Exception do: [:e | cleanup value. e pass]`, which misses every unwind
 that is not an exception: a non-local return (^ out of a block) straight
 through the protected frame ran NO cleanup, and Process terminate ran NO
 cleanup either. The emulation also ran the cleanup BEFORE the outer handler
 had decided to handle the exception (ANSI: cleanups run during the unwind,
 after the handler decision). Each section notes whether it FAILS on the
 pre-fix HEAD (falsifiability) or is a pinned regression.

 Harness: an Assert failure or uncaught error prints but still exits 0, so the
 last block caret-returns the failure count (0 = pass); see BooleanGlobalTest."

UnwindError := Error [
]


"An exception class whose class-side handles: logs the moment the handler
 search DECIDES this handler matches, so tests can order the handler decision
 against the ensure: cleanup. UnwindDecisionLog is a global (auto-created by
 being referenced from methods; assigned before use)."
UnwindProbeError := Error [

	class handles: anException [
		UnwindDecisionLog isNil ifFalse: [UnwindDecisionLog add: #decision].
		^anException isKindOf: self
	]

]


"Non-local-return probes. NLR needs a home method frame to return from, so
 each probe wraps the ^ in a block run by do: inside an instance method."
UnwindNlrProbe := Object [

	nlrThroughEnsure: log [
		"FAILS pre-fix: answers #early with log left EMPTY"
		#(1 2 3) do: [:each |
			[^#early] ensure: [log add: #cleanup]].
		^#late
	]


	nlrNested: log [
		"FAILS pre-fix: cleanups must run inside-out during the unwind"
		#(1) do: [:each |
			[[^#early] ensure: [log add: #inner]] ensure: [log add: #outer]].
		^#late
	]


	nlrCurtailed: log [
		"FAILS pre-fix: ifCurtailed: must fire on a non-local return"
		#(1) do: [:each |
			[^#early] ifCurtailed: [log add: #curtailed]].
		^#late
	]


	nlrFromLoop: log [
		"NLR out of an inlined whileTrue: body, through ensure:. Pre-fix the
		 iteration that returns runs no cleanup (log stops at 2)."
		| i |
		i := 0.
		[i < 5] whileTrue: [
			i := i + 1.
			[i = 3 ifTrue: [^i]] ensure: [log add: i]].
		^0
	]


	nlrValueUnderAllocation: log [
		"the returned value must survive scavenges triggered by the cleanup"
		| junk |
		#(1) do: [:each |
			[^'sur', 'vived']
				ensure: [
					1 to: 30000 do: [:i | junk := Array new: 8].
					log add: #survived]].
		^#late
	]


	nlrThroughHandler: log [
		"NLR straight through an ACTIVE on:do: frame: the ensure below it must
		 still run, the abandoned handler must never fire"
		#(1) do: [:each |
			[[^#early] ensure: [log add: #cleanup]]
				on: UnwindError do: [:e | log add: #wrongHandler]].
		^#late
	]

]


UnwindTest := Object [

	class report: aCount section: aName [
		aCount > 0 ifTrue: [
			Transcript nextPutAll: 'UnwindTest section '; nextPutAll: aName;
				nextPutAll: ' FAILURES='; nextPutAll: aCount printString; lf].
		^aCount
	]


	class nlrBasics [
		"FAILS pre-fix (cleanup never runs on NLR)"
		| fails log probe result |
		fails := 0.
		probe := UnwindNlrProbe new.

		log := OrderedCollection new.
		result := probe nlrThroughEnsure: log.
		(result == #early) ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].
		(log size = 1) ifTrue: [
			((log at: 1) == #cleanup) ifFalse: [fails := fails + 1]].

		log := OrderedCollection new.
		result := probe nlrCurtailed: log.
		(result == #early) ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].

		"pinned: ifCurtailed: must NOT fire on normal completion"
		log := OrderedCollection new.
		result := [42] ifCurtailed: [log add: #no].
		(result = 42) ifFalse: [fails := fails + 1].
		log isEmpty ifFalse: [fails := fails + 1].

		"pinned: ensure: still fires once on normal completion"
		log := OrderedCollection new.
		result := [7] ensure: [log add: #done].
		(result = 7) ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].
		^fails
	]


	class nlrNestedOrder [
		"FAILS pre-fix: nested cleanups, inside-out order"
		| fails log probe result |
		fails := 0.
		probe := UnwindNlrProbe new.
		log := OrderedCollection new.
		result := probe nlrNested: log.
		(result == #early) ifFalse: [fails := fails + 1].
		(log size = 2) ifFalse: [fails := fails + 1].
		(log size = 2) ifTrue: [
			((log at: 1) == #inner) ifFalse: [fails := fails + 1].
			((log at: 2) == #outer) ifFalse: [fails := fails + 1]].
		^fails
	]


	class nlrLoop [
		"FAILS pre-fix: the returning iteration must still run its cleanup"
		| fails log probe result |
		fails := 0.
		probe := UnwindNlrProbe new.
		log := OrderedCollection new.
		result := probe nlrFromLoop: log.
		(result = 3) ifFalse: [fails := fails + 1].
		(log size = 3) ifFalse: [fails := fails + 1].
		(log size = 3) ifTrue: [
			((log at: 3) = 3) ifFalse: [fails := fails + 1]].
		^fails
	]


	class nlrValueSurvivesGc [
		"FAILS pre-fix (cleanup never runs); post-fix also proves the NLR value
		 stays valid across cleanup-triggered scavenges"
		| fails log probe result |
		fails := 0.
		probe := UnwindNlrProbe new.
		log := OrderedCollection new.
		result := probe nlrValueUnderAllocation: log.
		(result = 'survived') ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].
		^fails
	]


	class nlrThroughHandler [
		"FAILS pre-fix; also pins that a later signal still finds live handlers
		 after an on:do: frame was abandoned by NLR"
		| fails log probe result |
		fails := 0.
		probe := UnwindNlrProbe new.
		log := OrderedCollection new.
		result := probe nlrThroughHandler: log.
		(result == #early) ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].
		(log includes: #wrongHandler) ifTrue: [fails := fails + 1].
		result := [UnwindError signal. #notCaught] on: UnwindError do: [:e | #caught].
		(result == #caught) ifFalse: [fails := fails + 1].
		^fails
	]


	class exceptionOrder [
		"ANSI order, final since milestone 2 (signal without unwind): the
		 handler DECISION (class-side handles: match) comes first, then the
		 handler BODY runs on top of the signaling frames, and the ensure:
		 cleanup runs last, during the unwind the handler's completion
		 triggers. (The pre-milestone-1 emulation ran cleanup before either;
		 milestone 1 ran it between decision and body.)"
		| fails result |
		fails := 0.
		UnwindDecisionLog := OrderedCollection new.
		result := [[UnwindProbeError signal] ensure: [UnwindDecisionLog add: #cleanup]]
			on: UnwindProbeError do: [:e | UnwindDecisionLog add: #handler. #handled].
		(result == #handled) ifFalse: [fails := fails + 1].
		(UnwindDecisionLog size = 3) ifFalse: [fails := fails + 1].
		(UnwindDecisionLog size = 3) ifTrue: [
			((UnwindDecisionLog at: 1) == #decision) ifFalse: [fails := fails + 1].
			((UnwindDecisionLog at: 2) == #handler) ifFalse: [fails := fails + 1].
			((UnwindDecisionLog at: 3) == #cleanup) ifFalse: [fails := fails + 1]].
		UnwindDecisionLog := nil.
		^fails
	]


	class exceptionNestedOrder [
		"nested cleanups run inside-out, exactly once each. ANSI order, final
		 since milestone 2: the handler body runs FIRST (no unwind at signal
		 time), the cleanups run when its completion unwinds."
		| fails log result |
		fails := 0.
		log := OrderedCollection new.
		result := [[[UnwindError signal] ensure: [log add: #inner]] ensure: [log add: #outer]]
			on: UnwindError do: [:e | log add: #handler. #h].
		(result == #h) ifFalse: [fails := fails + 1].
		(log size = 3) ifFalse: [fails := fails + 1].
		(log size = 3) ifTrue: [
			((log at: 1) == #handler) ifFalse: [fails := fails + 1].
			((log at: 2) == #inner) ifFalse: [fails := fails + 1].
			((log at: 3) == #outer) ifFalse: [fails := fails + 1]].
		^fails
	]


	class terminateSelf [
		"FAILS pre-fix: Processor thisProcess terminate must run pending ensures"
		| fails flag tries |
		fails := 0.
		flag := false.
		[[Processor thisProcess terminate] ensure: [flag := true]] fork.
		tries := 0.
		[flag not and: [tries < 20000]] whileTrue: [
			tries := tries + 1.
			Processor yield].
		flag ifFalse: [fails := fails + 1].
		^fails
	]


	class terminateParked [
		"FAILS pre-fix: terminating a parked fiber must run its pending ensures.
		 terminate is retried because a fiber still RUNNING on another worker is
		 (documented) not terminable; once it parks the terminate lands."
		| fails started flag tries process |
		fails := 0.
		started := false.
		flag := false.
		process := [[started := true. Processor suspendCurrent] ensure: [flag := true]] fork.
		tries := 0.
		[started not and: [tries < 20000]] whileTrue: [
			tries := tries + 1.
			Processor yield].
		started ifFalse: [^fails + 1].
		tries := 0.
		[flag not and: [tries < 20000]] whileTrue: [
			tries := tries + 1.
			process terminate.
			Processor yield].
		flag ifFalse: [fails := fails + 1].
		^fails
	]


	class unhandledInFiber [
		"pinned regression (also passes pre-fix, where the emulation caught the
		 exception): an UNHANDLED exception in a fiber must still run pending
		 ensures before the fiber dies. Post-fix this exercises defaultAction ->
		 terminate -> unwind. Prints the intentional backtrace noise.
		 The probe DELIBERATELY drives defaultAction, which counts an unhandled
		 error that would fail this run's exit code: take the counter, ASSERT
		 the death was recorded exactly once, and leave the run green."
		| fails flag tries taken |
		fails := 0.
		flag := false.
		[[UnwindError signal: 'intentional: unhandled in fiber probe'] ensure: [flag := true]] fork.
		tries := 0.
		[flag not and: [tries < 20000]] whileTrue: [
			tries := tries + 1.
			Processor yield].
		flag ifFalse: [fails := fails + 1].
		taken := Processor takeUnhandledErrorCount.
		taken = 1 ifFalse: [
			Transcript nextPutAll: 'unhandledInFiber: expected 1 recorded unhandled error, got '.
			Transcript nextPutAll: taken printString; lf.
			fails := fails + 1].
		^fails
	]


	class runAll [
		| total |
		total := self report: self nlrBasics section: 'nlrBasics'.
		total := total + (self report: self nlrNestedOrder section: 'nlrNestedOrder').
		total := total + (self report: self nlrLoop section: 'nlrLoop').
		total := total + (self report: self nlrValueSurvivesGc section: 'nlrValueSurvivesGc').
		total := total + (self report: self nlrThroughHandler section: 'nlrThroughHandler').
		total := total + (self report: self exceptionOrder section: 'exceptionOrder').
		total := total + (self report: self exceptionNestedOrder section: 'exceptionNestedOrder').
		total := total + (self report: self terminateSelf section: 'terminateSelf').
		total := total + (self report: self terminateParked section: 'terminateParked').
		total := total + (self report: self unhandledInFiber section: 'unhandledInFiber').
		total > 0 ifTrue: [
			Transcript nextPutAll: 'UnwindTest FAILURES='; nextPutAll: total printString; lf].
		^total
	]

]


[
	^UnwindTest runAll
]
