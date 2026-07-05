"============================================================
  Concurrency 03 - A user registry built from actors
  ------------------------------------------------------------
  A more realistic actor program. Each user is an *actor* whose
  private state is a rich domain object (a `User` instance with
  registration data). A `UserRegistry` actor is the parent that
  spawns and supervises one UserAccount per registered user.

  It shows:
    * actor state that is an instance of another class
    * a supervising parent that spawns children on demand
    * typed command messages (RegisterUser / SetEmail)
    * request/reply to read state back synchronously
    * 'let it crash': a failing account is restarted by the
      registry, reset to its original registration data
  ============================================================"

"---- Domain object: the state each UserAccount actor owns ----"
User := Object [
	| name email age active |

	class name: aName email: anEmail age: anAge [
		^self new setName: aName email: anEmail age: anAge
	]

	setName: aName email: anEmail age: anAge [
		name := aName. email := anEmail. age := anAge. active := true.
		^self
	]

	name [ ^name ]
	email [ ^email ]
	age [ ^age ]
	active [ ^active ]

	birthday [ age := age + 1 ]
	email: anEmail [ email := anEmail ]
	deactivate [ active := false ]

	describe [
		^name , ' <' , email , '> age ' , age printString ,
			(active ifTrue: [' (active)'] ifFalse: [' (inactive)'])
	]
]

"---- Command messages (typed, ProtoActor style) ----"
RegisterUser := Object [
	| name email age |
	class name: aName email: anEmail age: anAge [
		^self new setName: aName email: anEmail age: anAge ]
	setName: aName email: anEmail age: anAge [
		name := aName. email := anEmail. age := anAge. ^self ]
	name [ ^name ]  email [ ^email ]  age [ ^age ]
]

SetEmail := Object [
	| email |
	class to: anEmail [ ^self new setEmail: anEmail ]
	setEmail: anEmail [ email := anEmail. ^self ]
	email [ ^email ]
]

"---- The actor that owns one User ----"
UserAccount := Actor [
	| user |

	class forName: aName email: anEmail age: anAge [
		^self new setUserName: aName email: anEmail age: anAge
	]

	setUserName: aName email: anEmail age: anAge [
		user := User name: aName email: anEmail age: anAge.
		^self
	]

	receiveMessage: ctx [
		| m |
		m := ctx message.
		(m isKindOf: SetEmail) ifTrue: [ ^user email: m email ].
		m = #birthday   ifTrue: [ ^user birthday ].
		m = #deactivate ifTrue: [ ^user deactivate ].
		m = #describe   ifTrue: [ ^ctx respond: user describe ].
		m = #crash      ifTrue: [ ^self error: 'account corruption!' ].
	]
]

"---- The registry: spawns and supervises the accounts ----"
UserRegistry := Actor [
	| accounts |

	started: ctx [ accounts := OrderedCollection new ]

	receiveMessage: ctx [
		| m |
		m := ctx message.
		(m isKindOf: RegisterUser) ifTrue: [
			| name email age pid |
			name := m name. email := m email. age := m age.
			pid := ctx spawn: (Props fromProducer: [
				UserAccount forName: name email: email age: age ]).
			accounts add: pid.
			^ctx respond: pid ].
		m = #count ifTrue: [ ^ctx respond: accounts size ].
		m = #deactivateAll ifTrue: [ ^accounts do: [:acc | acc tell: #deactivate] ].
	]
]


"---- Demo -----------------------------------------------------------------"
[
	| registry alice bob carol |
	registry := Actors spawn: (Props fromActorClass: UserRegistry).

	"register three users; each request returns the new account's Pid"
	alice := (registry request: (RegisterUser name: 'Alice' email: 'alice@mail.com' age: 30)) await.
	bob   := (registry request: (RegisterUser name: 'Bob'   email: 'bob@mail.com'   age: 25)) await.
	carol := (registry request: (RegisterUser name: 'Carol' email: 'carol@mail.com' age: 40)) await.

	Transcript nextPutAll: 'registered users: '. (registry request: #count) await printNl.

	"mutate Alice: two birthdays and a new email"
	alice tell: #birthday.
	alice tell: #birthday.
	alice tell: (SetEmail to: 'alice@work.com').
	Transcript nextPutAll: 'alice -> '. (alice request: #describe) await printNl.

	Transcript nextPutAll: 'bob   -> '. (bob request: #describe) await printNl.

	"registry broadcasts to every child"
	registry tell: #deactivateAll.
	1 to: 4 do: [:i | Processor yield].
	Transcript nextPutAll: 'carol -> '. (carol request: #describe) await printNl.

	"let it crash: Alice's account fails and the registry restarts it,
	 resetting the account to its original registration data"
	Transcript nextPutAll: 'crashing alice''s account...'; lf.
	alice tell: #crash.
	Processor yield.
	Transcript nextPutAll: 'alice after restart -> '. (alice request: #describe) await printNl.
	Transcript nextPutAll: '(age and email are back to registration values)'; lf.

	registry stop.
]
