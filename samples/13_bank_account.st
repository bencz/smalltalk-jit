"============================================================
  13 - Bank Account (OOP + domain exceptions)
  ------------------------------------------------------------
  A small domain model: an Account with deposit/withdraw, a
  transaction history, and a custom exception raised when a
  withdrawal would overdraw the balance.
  ============================================================"

InsufficientFunds := Error [
	| requested available |
	class requested: r available: a [
		^self new setRequested: r available: a
	]
	setRequested: r available: a [ requested := r. available := a ]
	messageText [
		^'cannot withdraw ' , requested printString ,
		 ' (balance is only ' , available printString , ')'
	]
]

Account := Object [
	| owner balance history |

	class for: aName [ ^self new initFor: aName ]

	initFor: aName [
		owner := aName.
		balance := 0.
		history := OrderedCollection new.
	]

	balance [ ^balance ]
	owner [ ^owner ]

	deposit: amount [
		balance := balance + amount.
		history add: 'deposit ' , amount printString.
		^balance
	]

	withdraw: amount [
		amount > balance ifTrue: [
			(InsufficientFunds requested: amount available: balance) signal].
		balance := balance - amount.
		history add: 'withdraw ' , amount printString.
		^balance
	]

	printHistory [
		Transcript nextPutAll: 'Statement for '; nextPutAll: owner; nextPutAll: ':'; lf.
		history do: [:line |
			Transcript nextPutAll: '  '; nextPutAll: line; lf].
		Transcript nextPutAll: '  balance = '; nextPutAll: balance printString; lf.
	]
]

[
	| acc |
	acc := Account for: 'Alice'.
	acc deposit: 100.
	acc deposit: 50.
	acc withdraw: 30.

	"This withdrawal is too big - handle the exception gracefully."
	[acc withdraw: 500]
		on: InsufficientFunds
		do: [:e |
			Transcript nextPutAll: 'DENIED: '; nextPutAll: e messageText; lf].

	acc withdraw: 20.
	acc printHistory.
]
