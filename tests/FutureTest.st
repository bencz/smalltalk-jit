"=== Standard test harness. Paste verbatim at the top of each tests/*.st file.
 Every check is wrapped so an unexpected runtime error counts as a FAILURE
 (a raw Assert failure or uncaught error prints a backtrace yet still exits 0,
 i.e. a false pass). The final top-level block MUST end with ^t report, whose
 integer return becomes the process exit code (0 = pass) that run_tests.sh
 gates on. Do not use the old Assert class in new tests. All catches use
 Exception (the root) so a wrong error-class name surfaces as a visible FAIL,
 never a silent exit-0. ==="
TestRun := Object [
    | fails count name |
    class named: aString [ ^self new setName: aString ]
    setName: aString [ name := aString. fails := 0. count := 0. ^self ]
    fail: aLabel [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' FAIL: '; nextPutAll: aLabel; lf.
    ]
    error: aLabel from: e [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' ERROR ['; nextPutAll: aLabel;
            nextPutAll: ']: '; nextPutAll: e messageText; lf.
    ]
    check: aLabel that: aBlock [
        count := count + 1.
        [ (aBlock value = true) ifFalse: [ self fail: aLabel ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    check: aLabel equals: expected is: aBlock [
        count := count + 1.
        [ | got |
          got := aBlock value.
          got = expected ifFalse: [
              self fail: aLabel, ' (got ', got printString, ' expected ', expected printString, ')' ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    expect: aLabel raises: aClass in: aBlock [
        count := count + 1.
        [ aBlock value. self fail: aLabel, ' (no error raised)' ]
            on: Exception do: [:e |
                (aClass notNil and: [ e isKindOf: aClass ]) ifFalse: [
                    self fail: aLabel, ' (raised ', e class printString, ' not ', aClass printString, ')' ] ].
    ]
    report [
        Transcript nextPutAll: name; nextPutAll: ': ';
            nextPutAll: (count - fails) printString; nextPutAll: '/';
            nextPutAll: count printString; nextPutAll: ' ok'; lf.
        ^fails
    ]
]

"Deterministic tests for Future, the reply side of request/response. The Future
 API is small: pid, complete:, await, awaitFor:, and the endpoint hooks
 postUser:sender:/postSystem:. There is NO isResolved/then:/onComplete, so those
 are not tested. Most checks complete the future synchronously on the main
 process FIRST and then await, so await returns immediately without parking (no
 fibers, fully deterministic). The one genuinely-async pattern (request/await
 through a live actor, and the awaitFor: timeout) mirrors ActorTest exactly and
 stays bounded."

"--- helper actor: answers requests so we can obtain real Futures ---------"

FutureProbe := Actor [
	| v |
	started: ctx [ v := 10 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #get ifTrue: [ ctx respond: v ].
		(m isKindOf: SmallInteger) ifTrue: [ ctx respond: m * 2 ].
	]
]


"--- driver: ONE TestRun, its ^report becomes the exit code --------------"

[
	| t |
	t := TestRun named: 'Future'.

	"------------------------------------------------ construction & pid"

	t check: 'Future in: answers a Future with a Pid' that: [
		| f | f := Future in: Actors. f pid isKindOf: Pid ].
	t check: 'a Future carries a direct pid pointing back at itself' that: [
		| f | f := Future in: Actors. (f pid endpoint) == f ].
	t check: 'a Future pid system is the global Actors' that: [
		| f | f := Future in: Actors. f pid system == Actors ].
	t check: 'distinct Futures get distinct pids' that: [
		| a b | a := Future in: Actors. b := Future in: Actors. (a pid = b pid) = false ].

	"------------------------------------------------ complete / await"

	t check: 'complete: then await returns the value' equals: 42 is: [
		| f | f := Future in: Actors. f complete: 42. f await ].
	t check: 'await after complete: does not block a non-integer value' that: [
		| f | f := Future in: Actors. f complete: #ok. f await == #ok ].
	t check: 'double complete: keeps the first value' equals: 1 is: [
		| f | f := Future in: Actors. f complete: 1. f complete: 2. f await ].
	t check: 'awaitFor: on an already-completed Future returns the value' equals: 5 is: [
		| f | f := Future in: Actors. f complete: 5. f awaitFor: 1000 ].

	"------------------------------------------------ endpoint hooks"

	t check: 'postUser:sender: completes the Future' equals: 7 is: [
		| f | f := Future in: Actors. f postUser: 7 sender: nil. f await ].
	t check: 'postUser:sender: twice keeps the first value' equals: 7 is: [
		| f | f := Future in: Actors.
		f postUser: 7 sender: nil. f postUser: 8 sender: nil. f await ].
	t check: 'postSystem: does not complete the Future' equals: 9 is: [
		| f | f := Future in: Actors. f postSystem: SystemStop. f complete: 9. f await ].

	"------------------------------------------------ real request/await path"

	t check: 'system request answers a Future' that: [
		| a fut r | a := Actors spawn: (Props fromActorClass: FutureProbe).
		fut := a request: #get.
		r := fut isKindOf: Future.
		fut await. a stop. r ].
	t check: 'request/await round-trips the actor reply' equals: 10 is: [
		| a got | a := Actors spawn: (Props fromActorClass: FutureProbe).
		got := (a request: #get) await. a stop. got ].
	t check: 'request/await carries a computed reply' equals: 42 is: [
		| a got | a := Actors spawn: (Props fromActorClass: FutureProbe).
		got := (a request: 21) await. a stop. got ].

	"------------------------------------------------ awaitFor: timeout"
	"Bounded, mirrors ActorTest: a Future that is never completed answers nil
	 after the timeout. Uses the same 50ms budget ActorTest uses."

	t check: 'awaitFor: on a never-completed Future times out to nil' that: [
		| f | f := Future in: Actors. (f awaitFor: 50) isNil ].

	^t report
]
