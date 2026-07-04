"============================================================
  ADVANCED - Chain of Responsibility
  ------------------------------------------------------------
  Give more than one object a chance to handle a request by
  chaining the handlers and passing the request along the chain
  until someone deals with it.

  Example: a support desk. A ticket with a difficulty level is
  offered to Basic support, then a Supervisor, then a Manager.
  Each handles what it can and forwards the rest.
  ============================================================"

SupportHandler := Object [
	| next |

	setNext: aHandler [ next := aHandler ]

	handle: level [
		(self canHandle: level)
			ifTrue:  [self process: level]
			ifFalse: [
				next isNil
					ifTrue:  [Transcript nextPutAll: '  ticket L'; nextPutAll: level printString;
								nextPutAll: ' fell off the end of the chain!'; lf]
					ifFalse: [next handle: level]]
	]

	canHandle: level [ ^self subclassResponsibility ]
	process: level [ ^self subclassResponsibility ]
]

BasicSupport := SupportHandler [
	canHandle: level [ ^level <= 1 ]
	process: level [
		Transcript nextPutAll: '  L'; nextPutAll: level printString;
			nextPutAll: ' handled by Basic Support'; lf.
	]
]

Supervisor := SupportHandler [
	canHandle: level [ ^level <= 2 ]
	process: level [
		Transcript nextPutAll: '  L'; nextPutAll: level printString;
			nextPutAll: ' handled by Supervisor'; lf.
	]
]

Manager := SupportHandler [
	canHandle: level [ ^level <= 3 ]
	process: level [
		Transcript nextPutAll: '  L'; nextPutAll: level printString;
			nextPutAll: ' handled by Manager'; lf.
	]
]

[
	| basic supervisor manager |
	"Build the chain: Basic -> Supervisor -> Manager."
	basic := BasicSupport new.
	supervisor := Supervisor new.
	manager := Manager new.
	basic setNext: supervisor.
	supervisor setNext: manager.

	"Route several tickets of varying difficulty through it."
	#(1 2 3 1 2 4) do: [:level |
		Transcript nextPutAll: 'incoming ticket, difficulty '; nextPutAll: level printString; lf.
		basic handle: level].
]
