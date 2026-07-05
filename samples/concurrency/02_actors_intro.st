"============================================================
  Concurrency 02 - Actors (introduction)
  ------------------------------------------------------------
  An actor is an object with a private mailbox served by its own
  process. You never touch an actor directly: you hold its Pid
  and send it messages. Because only its own process ever runs
  its methods, an actor's state needs no locks.

  Subclass Actor and implement `receiveMessage:` (and optional
  lifecycle hooks started:/stopping:/stopped:/restarting:).
  Drive it with `tell:` (fire-and-forget) or `request:` (returns
  a Future you `await`).

  This sample shows: request/reply, safe shared state under
  concurrency, and 'let it crash' supervision.
  ============================================================"

"---- A bank account actor: state is a running balance ----"
BankAccount := Actor [
	| balance |
	started: ctx [ balance := 0 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		(m isKindOf: Association)
			ifTrue: [
				m key = #deposit  ifTrue: [ balance := balance + m value ].
				m key = #withdraw ifTrue: [
					m value <= balance ifTrue: [ balance := balance - m value ] ] ]
			ifFalse: [
				m = #balance ifTrue: [ ctx respond: balance ] ].
	]
]

"---- A flaky worker to demonstrate supervision ----"
Flaky := Actor [
	| handled |
	started: ctx [ handled := 0 ]
	restarting: ctx [
		Transcript nextPutAll: '  (supervisor is restarting Flaky after a crash)'; lf ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #boom ifTrue: [ self error: 'simulated failure' ].
		m = #work ifTrue: [ handled := handled + 1. ctx respond: handled ].
	]
]


"---- 1. Concurrent deposits, then a synchronous balance read ----"
[
	| account |
	account := Actors spawn: (Props fromActorClass: BankAccount).

	"ten processes each deposit 100 concurrently; the actor serialises them"
	1 to: 10 do: [:w |
		[ 1 to: 10 do: [:i | account tell: (#deposit -> 100) ] ] fork ].

	"let the depositors run, then withdraw once"
	1 to: 30 do: [:i | Processor yield].
	account tell: (#withdraw -> 250).

	Transcript nextPutAll: 'final balance = '.
	(account request: #balance) await printNl.   "10*10*100 - 250 = 9750"
	account stop.
]


"---- 2. Let it crash: a supervised actor recovers automatically ----"
[
	| flaky |
	flaky := Actors spawn: (Props fromActorClass: Flaky).

	Transcript nextPutAll: 'work #1 -> '. (flaky request: #work) await printNl.   "1"
	Transcript nextPutAll: 'sending #boom (actor will crash)...'; lf.
	flaky tell: #boom.                    "crashes; the guardian restarts it"
	Processor yield.

	Transcript nextPutAll: 'work after restart -> '.
	(flaky request: #work) await printNl.  "1 again: fresh state, actor is alive"
	Transcript nextPutAll: 'the VM never noticed the crash.'; lf.
	flaky stop.
]
