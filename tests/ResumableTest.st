"ResumableTest: the resumable exception protocol (milestone 2).

 The signal must run the matching handler ON TOP of the signaling frames,
 without unwinding: unwinding happens only when the handler completes normally
 or uses return:/retry/retryUsing:; resume: aValue makes the original signal
 expression answer aValue and execution continue from the signal point (legal
 only when isResumable). Error is non-resumable; Warning is a new resumable
 class whose default action prints and resumes nil; outer/pass/resignalAs: get
 real semantics.

 Falsifiability: on the pre-milestone-2 HEAD (real unwind for ensure:, but
 signal still unwinds before the handler runs and resume: raises), every
 section below marked FAILS pre-fix was verified to fail. Risky expressions
 are wrapped in on: Exception so a pre-fix run counts failures instead of
 killing the runner (an uncaught error exits 0, see BooleanGlobalTest).

 Harness: the last block caret-returns the failure count (0 = pass)."

UnwResumable := Exception [
]


UnwErrorA := Error [
]


UnwErrorB := Error [
]


ResumableTest := Object [

	class report: aCount section: aName [
		aCount > 0 ifTrue: [
			Transcript nextPutAll: 'ResumableTest section '; nextPutAll: aName;
				nextPutAll: ' FAILURES='; nextPutAll: aCount printString; lf].
		^aCount
	]


	class resumeValue [
		"FAILS pre-fix: resume: answers the value AT the signal point and
		 execution continues from there"
		| fails r log |
		fails := 0.
		r := [[(UnwResumable new signal) + 1] on: UnwResumable do: [:e | e resume: 41]]
			on: Exception do: [:x | #err].
		(r = 42) ifFalse: [fails := fails + 1].

		log := OrderedCollection new.
		r := [[log add: #before. log add: UnwResumable new signal. #done]
				on: UnwResumable do: [:e | e resume: 99]]
			on: Exception do: [:x | #err].
		(r == #done) ifFalse: [fails := fails + 1].
		(log size = 2) ifFalse: [fails := fails + 1].
		(log size = 2) ifTrue: [
			((log at: 1) == #before) ifFalse: [fails := fails + 1].
			((log at: 2) = 99) ifFalse: [fails := fails + 1]].
		^fails
	]


	class resumeSurvivesGc [
		"FAILS pre-fix; post-fix also proves the signal activation state (saved
		 handler chain, handler record) survives handler-triggered scavenges"
		| fails r junk |
		fails := 0.
		r := [[UnwResumable new signal]
				on: UnwResumable do: [:e |
					1 to: 30000 do: [:i | junk := Array new: 8].
					e resume: 'o', 'k']]
			on: Exception do: [:x | #err].
		(r = 'ok') ifFalse: [fails := fails + 1].
		^fails
	]


	class resumeOnErrorRaises [
		"pinned: resume: on a non-resumable exception raises an Error (this also
		 passed pre-fix, where EVERY resume: raised)"
		| fails r |
		fails := 0.
		r := [[Error signal]
				on: Error do: [:e |
					[e resume: 1. #noRaise] on: Error do: [:x | #raised]]]
			on: Exception do: [:x | #outerErr].
		(r == #raised) ifFalse: [fails := fails + 1].
		(Error new isResumable) ifTrue: [fails := fails + 1].
		^fails
	]


	class warningContinues [
		"FAILS pre-fix (Warning did not exist): an UNHANDLED Warning prints its
		 message and resumes nil, so the method carries on"
		| fails r log |
		fails := 0.
		log := OrderedCollection new.
		"wrapped in on: Error (NOT on: Exception, which would catch the Warning
		 itself and defeat the unhandled path): pre-fix the nil Warning global
		 raised a DNU, an Error"
		r := [log add: #before.
			log add: (Warning signal: 'intentional warning probe (ResumableTest)').
			log add: #after.
			#done]
			on: Error do: [:x | #err].
		(r == #done) ifFalse: [fails := fails + 1].
		(log size = 3) ifFalse: [fails := fails + 1].
		(log size = 3) ifTrue: [
			((log at: 2) isNil) ifFalse: [fails := fails + 1].
			((log at: 3) == #after) ifFalse: [fails := fails + 1]].

		"a HANDLED Warning follows the handler's decision. Guarded: pre-fix the
		 Warning global is an auto-created nil, and `on: nil do:` would corrupt
		 the handler search (handles: sent to nil) and kill the runner."
		Warning isNil
			ifTrue: [fails := fails + 2]
			ifFalse: [
				r := [[Warning signal: 'handled warning'] on: Warning do: [:e | e resume: 5]]
					on: Exception do: [:x | #err].
				(r = 5) ifFalse: [fails := fails + 1].
				(Warning new isResumable) ifFalse: [fails := fails + 1]].
		^fails
	]


	class resumeSkipsCleanups [
		"FAILS pre-fix: a handler that resumes must NOT run the intermediate
		 ifCurtailed:/unwind cleanups (no unwind happened); a handler that
		 returns MUST. ensure: still fires exactly once via normal completion
		 after a resumption."
		| fails r log |
		fails := 0.
		log := OrderedCollection new.
		r := [[[UnwResumable new signal] ifCurtailed: [log add: #curtailed]]
				on: UnwResumable do: [:e | e resume: 7]]
			on: Exception do: [:x | #err].
		(r = 7) ifFalse: [fails := fails + 1].
		log isEmpty ifFalse: [fails := fails + 1].

		log := OrderedCollection new.
		r := [[[UnwResumable new signal. #notReached] ifCurtailed: [log add: #curtailed]]
				on: UnwResumable do: [:e | e return: 8]]
			on: Exception do: [:x | #err].
		(r = 8) ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].

		log := OrderedCollection new.
		r := [[[UnwResumable new signal] ensure: [log add: #cleanup]]
				on: UnwResumable do: [:e | e resume: 9]]
			on: Exception do: [:x | #err].
		(r = 9) ifFalse: [fails := fails + 1].
		(log size = 1) ifFalse: [fails := fails + 1].
		^fails
	]


	class retryAndRetryUsing [
		"retry re-runs the protected block (worked before); retryUsing: is new,
		 FAILS pre-fix: it evaluates the replacement block instead"
		| fails r count |
		fails := 0.
		count := 0.
		r := [count := count + 1. count < 3 ifTrue: [UnwErrorA new signal]. count]
			on: UnwErrorA do: [:e | e retry].
		(r = 3) ifFalse: [fails := fails + 1].
		(count = 3) ifFalse: [fails := fails + 1].

		count := 0.
		r := [[count := count + 1. UnwErrorA new signal. #original]
				on: UnwErrorA do: [:e | e retryUsing: [#replacement]]]
			on: Exception do: [:x | #err].
		(r == #replacement) ifFalse: [fails := fails + 1].
		(count = 1) ifFalse: [fails := fails + 1].
		^fails
	]


	class resignalAs [
		"FAILS pre-fix (resignalAs: did not exist): the replacement exception is
		 signaled from the ORIGINAL signal environment, so even the handler that
		 resignaled is eligible again"
		| fails r log seen |
		fails := 0.
		log := OrderedCollection new.
		r := [[[UnwErrorA new signal]
				on: UnwErrorA do: [:e | log add: #a. e resignalAs: UnwErrorB new]]
				on: UnwErrorB do: [:e2 | log add: #b. #handledB]]
			on: Exception do: [:x | #err].
		(r == #handledB) ifFalse: [fails := fails + 1].
		(log size = 2) ifFalse: [fails := fails + 1].
		(log size = 2) ifTrue: [
			((log at: 1) == #a) ifFalse: [fails := fails + 1].
			((log at: 2) == #b) ifFalse: [fails := fails + 1]].

		"the resignaling handler itself sees the replacement"
		seen := OrderedCollection new.
		r := [[UnwErrorA new signal]
				on: Error do: [:e |
					seen add: e class.
					(e isKindOf: UnwErrorB)
						ifTrue: [#done2]
						ifFalse: [e resignalAs: UnwErrorB new]]]
			on: Exception do: [:x | #err].
		(r == #done2) ifFalse: [fails := fails + 1].
		(seen size = 2) ifFalse: [fails := fails + 1].
		(seen size = 2) ifTrue: [
			((seen at: 1) == UnwErrorA) ifFalse: [fails := fails + 1].
			((seen at: 2) == UnwErrorB) ifFalse: [fails := fails + 1]].
		^fails
	]


	class outerResumeReturnsToInner [
		"FAILS pre-fix: when the enclosing handler resumes, #outer answers that
		 value INSIDE the inner handler, which then carries on"
		| fails r log v |
		fails := 0.
		log := OrderedCollection new.
		r := [[[UnwResumable new signal]
				on: UnwResumable do: [:e |
					log add: #innerBefore.
					v := e outer.
					log add: #innerAfter.
					v + 1]]
				on: UnwResumable do: [:e2 | e2 resume: 10]]
			on: Exception do: [:x | #err].
		(r = 11) ifFalse: [fails := fails + 1].
		(log size = 2) ifFalse: [fails := fails + 1].
		(log size = 2) ifTrue: [
			((log at: 1) == #innerBefore) ifFalse: [fails := fails + 1].
			((log at: 2) == #innerAfter) ifFalse: [fails := fails + 1]].
		^fails
	]


	class passResumesOriginalSignal [
		"FAILS pre-fix: pass hands the decision outward; when the outer handler
		 resumes, the value goes to the ORIGINAL signal point and the passing
		 handler is abandoned"
		| fails r log |
		fails := 0.
		log := OrderedCollection new.
		r := [[[log add: UnwResumable new signal. #done]
				on: UnwResumable do: [:e | log add: #inner. e pass. log add: #never]]
				on: UnwResumable do: [:e2 | log add: #outer. e2 resume: 99]]
			on: Exception do: [:x | #err].
		(r == #done) ifFalse: [fails := fails + 1].
		(log includes: #never) ifTrue: [fails := fails + 1].
		(log size = 3) ifFalse: [fails := fails + 1].
		(log size = 3) ifTrue: [
			((log at: 1) == #inner) ifFalse: [fails := fails + 1].
			((log at: 2) == #outer) ifFalse: [fails := fails + 1].
			((log at: 3) = 99) ifFalse: [fails := fails + 1]].

		"pass with an outer handler that returns: normal outward handling
		 (pinned: also worked pre-fix)"
		r := [[[UnwErrorA new signal] on: UnwErrorA do: [:e | e pass]]
				on: UnwErrorA do: [:e2 | #outerHandled]]
			on: Exception do: [:x | #err].
		(r == #outerHandled) ifFalse: [fails := fails + 1].
		^fails
	]


	class nestedResumeRightLevel [
		"FAILS pre-fix: nested on:do: activations resume at the right level"
		| fails r log |
		fails := 0.
		log := OrderedCollection new.
		r := [[UnwErrorA new signal]
				on: UnwErrorA do: [:e |
					log add: #aHandler.
					log add: ([UnwResumable new signal]
						on: UnwResumable do: [:eb | eb resume: #rb]).
					#aDone]]
			on: Exception do: [:x | #err].
		(r == #aDone) ifFalse: [fails := fails + 1].
		(log size = 2) ifFalse: [fails := fails + 1].
		(log size = 2) ifTrue: [
			((log at: 2) == #rb) ifFalse: [fails := fails + 1]].

		"resuming the OUTER exception from inside the INNER handler abandons the
		 whole inner activation and answers at the outer signal point"
		r := [[UnwResumable new signal]
				on: UnwResumable do: [:ea |
					[UnwErrorA new signal]
						on: UnwErrorA do: [:eb | ea resume: 77. #neverB]]]
			on: Exception do: [:x | #err].
		(r = 77) ifFalse: [fails := fails + 1].
		^fails
	]


	class runAll [
		| total |
		total := self report: self resumeValue section: 'resumeValue'.
		total := total + (self report: self resumeSurvivesGc section: 'resumeSurvivesGc').
		total := total + (self report: self resumeOnErrorRaises section: 'resumeOnErrorRaises').
		total := total + (self report: self warningContinues section: 'warningContinues').
		total := total + (self report: self resumeSkipsCleanups section: 'resumeSkipsCleanups').
		total := total + (self report: self retryAndRetryUsing section: 'retryAndRetryUsing').
		total := total + (self report: self resignalAs section: 'resignalAs').
		total := total + (self report: self outerResumeReturnsToInner section: 'outerResumeReturnsToInner').
		total := total + (self report: self passResumesOriginalSignal section: 'passResumesOriginalSignal').
		total := total + (self report: self nestedResumeRightLevel section: 'nestedResumeRightLevel').
		total > 0 ifTrue: [
			Transcript nextPutAll: 'ResumableTest FAILURES='; nextPutAll: total printString; lf].
		^total
	]

]


[
	^ResumableTest runAll
]
