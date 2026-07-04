"============================================================
  ADVANCED - State Pattern
  ------------------------------------------------------------
  An object changes its behaviour when its internal state
  changes, as if it changed class. Each state is a separate
  object that knows how to react to events and which state to
  move to next.

  Example: a coin-operated Turnstile with two states, Locked
  and Unlocked, reacting to the events #coin and #push.
  ============================================================"

TurnstileState := Object [
	name [ ^self subclassResponsibility ]
	coin: aTurnstile [ ^self subclassResponsibility ]
	push: aTurnstile [ ^self subclassResponsibility ]
]

Locked := TurnstileState [
	name [ ^'Locked' ]
	coin: turnstile [
		Transcript nextPutAll: '    coin accepted -> unlocking'; lf.
		turnstile state: Unlocked new.
	]
	push: turnstile [
		Transcript nextPutAll: '    push denied (still locked)'; lf.
	]
]

Unlocked := TurnstileState [
	name [ ^'Unlocked' ]
	coin: turnstile [
		Transcript nextPutAll: '    extra coin returned'; lf.
	]
	push: turnstile [
		Transcript nextPutAll: '    walk through -> locking'; lf.
		turnstile state: Locked new.
	]
]

Turnstile := Object [
	| state |
	class new [ ^super new init ]
	init [ state := Locked new ]

	state: aState [ state := aState ]
	state [ ^state ]

	coin [ Transcript nextPutAll: '[', state name, '] coin'; lf. state coin: self ]
	push [ Transcript nextPutAll: '[', state name, '] push'; lf. state push: self ]
]

[
	| gate |
	gate := Turnstile new.
	"A sequence of events; watch the behaviour change with the state."
	gate push.     "denied"
	gate coin.     "unlocks"
	gate coin.     "already unlocked"
	gate push.     "through, locks again"
	gate push.     "denied again"
	Transcript nextPutAll: 'final state = '; nextPutAll: gate state name; lf.
]
