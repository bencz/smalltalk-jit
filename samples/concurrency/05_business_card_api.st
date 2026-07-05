"============================================================
  Concurrency 05 - A business-card API: HTTP + actors together
  ------------------------------------------------------------
  A real HTTP/1.1 server (HttpServer) whose handlers talk to an
  actor system. Every registered *user* is an actor, and every
  *business card* is an actor that remembers which user it belongs
  to and counts, in its own state, how many times its data has
  been requested.

  One request exercises the whole stack:
     socket (epoll) -> handler fiber -> actor mailbox -> channel
  and back. It all runs cooperatively on a single OS thread, so
  thousands of connections multiplex without threads or locks.

  Endpoints:
     POST /users                {name,email}   -> {id}
     GET  /users/:id                           -> profile + card ids
     POST /users/:id/cards      {title,...}     -> {id}
     GET  /cards/:id            (counts a view) -> card data + views
     GET  /cards/:id/views                      -> {views} (no count)
     GET  /health                               -> {status:ok}

  This file is self-contained: it starts the server, drives it with
  an in-VM HTTP client, runs a small concurrency burst to measure
  throughput, then shuts down and exits.
  ============================================================"

"---- Command messages the Directory understands (typed, ProtoActor style) ----"
RegisterUser := Object [
	| name email |
	class name: aName email: anEmail [ ^self new setName: aName email: anEmail ]
	setName: aName email: anEmail [ name := aName. email := anEmail. ^self ]
	name [ ^name ]  email [ ^email ]
]

CreateCard := Object [
	| owner fields |
	class owner: anOwner fields: aDict [ ^self new setOwner: anOwner fields: aDict ]
	setOwner: anOwner fields: aDict [ owner := anOwner. fields := aDict. ^self ]
	owner [ ^owner ]  fields [ ^fields ]
]

LookupUser := Object [
	| id |
	class id: anId [ ^self new setId: anId ]
	setId: anId [ id := anId. ^self ]
	id [ ^id ]
]

LookupCard := Object [
	| id |
	class id: anId [ ^self new setId: anId ]
	setId: anId [ id := anId. ^self ]
	id [ ^id ]
]

AddCard := Object [
	| cardId |
	class id: anId [ ^self new setId: anId ]
	setId: anId [ cardId := anId. ^self ]
	cardId [ ^cardId ]
]


"---- Each registered user is an actor ----"
UserActor := Actor [
	| id name email cardIds |

	class id: anId name: aName email: anEmail [
		^self new setId: anId name: aName email: anEmail
	]

	setId: anId name: aName email: anEmail [
		id := anId. name := aName. email := anEmail.
		cardIds := OrderedCollection new.
		^self
	]

	receiveMessage: ctx [
		| m |
		m := ctx message.
		(m isKindOf: AddCard) ifTrue: [ ^cardIds addLast: m cardId ].
		m = #profile ifTrue: [ ^ctx respond: self profile ].
	]

	profile [
		| d |
		d := Dictionary new.
		d at: 'id' put: id.
		d at: 'name' put: name.
		d at: 'email' put: email.
		d at: 'cards' put: cardIds asArray.
		^d
	]
]


"---- Each business card is an actor that counts how often it is read ----"
BusinessCardActor := Actor [
	| id ownerId fields views |

	class id: anId owner: anOwner fields: aDict [
		^self new setId: anId owner: anOwner fields: aDict
	]

	setId: anId owner: anOwner fields: aDict [
		id := anId. ownerId := anOwner. fields := aDict. views := 0.
		^self
	]

	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #view ifTrue: [
			views := views + 1.
			^ctx respond: self dataWithViews ].
		m = #stats ifTrue: [ ^ctx respond: self stats ].
	]

	"the full card, having just counted this read"
	dataWithViews [
		| d |
		d := Dictionary new.
		fields keysAndValuesDo: [:k :v | d at: k put: v ].
		d at: 'id' put: id.
		d at: 'owner' put: ownerId.
		d at: 'views' put: views.
		^d
	]

	"just the counter, without counting this read"
	stats [
		| d |
		d := Dictionary new.
		d at: 'id' put: id.
		d at: 'owner' put: ownerId.
		d at: 'views' put: views.
		^d
	]
]


"---- The Directory actor: id registry that spawns and resolves the others ----"
Directory := Actor [
	| users cards userSeq cardSeq |

	started: ctx [
		users := Dictionary new.
		cards := Dictionary new.
		userSeq := 0.
		cardSeq := 0.
	]

	receiveMessage: ctx [
		| m |
		m := ctx message.

		(m isKindOf: RegisterUser) ifTrue: [
			| uid pid |
			userSeq := userSeq + 1.
			uid := userSeq.
			pid := ctx spawn: (Props fromProducer: [
				UserActor id: uid name: m name email: m email ]).
			users at: uid put: pid.
			^ctx respond: uid ].

		(m isKindOf: CreateCard) ifTrue: [
			| ownerPid cid cardPid |
			ownerPid := users at: m owner ifAbsent: [ nil ].
			ownerPid isNil ifTrue: [ ^ctx respond: nil ].
			cardSeq := cardSeq + 1.
			cid := cardSeq.
			cardPid := ctx spawn: (Props fromProducer: [
				BusinessCardActor id: cid owner: m owner fields: m fields ]).
			cards at: cid put: cardPid.
			ownerPid tell: (AddCard id: cid).
			^ctx respond: cid ].

		(m isKindOf: LookupUser) ifTrue: [ ^ctx respond: (users at: m id ifAbsent: [ nil ]) ].
		(m isKindOf: LookupCard) ifTrue: [ ^ctx respond: (cards at: m id ifAbsent: [ nil ]) ].
	]
]


"---- Wire the HTTP routes onto the actor system ----"
BusinessCardApi := Object [

	class on: aServer directory: directory [
		"POST /users {name,email} -> 201 {id}"
		aServer post: '/users' do: [:req |
			| payload name uid |
			payload := req jsonBody.
			(payload isNil or: [ (payload at: 'name' ifAbsent: [ nil ]) isNil ])
				ifTrue: [ HttpResponse badRequest: 'name is required' ]
				ifFalse: [
					name := payload at: 'name'.
					uid := (directory request: (RegisterUser
						name: name
						email: (payload at: 'email' ifAbsent: [ '' ]))) await.
					HttpResponse created: (Dictionary new at: 'id' put: uid; yourself) ] ].

		"GET /users/:id -> 200 profile"
		aServer get: '/users/:id' do: [:req |
			| pid |
			pid := (directory request: (LookupUser id: (req param: 'id') asNumber)) await.
			pid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #profile) await) ] ].

		"POST /users/:id/cards {title,company,...} -> 201 {id}"
		aServer post: '/users/:id/cards' do: [:req |
			| payload cid |
			payload := req jsonBody.
			payload isNil ifTrue: [ payload := Dictionary new ].
			cid := (directory request: (CreateCard
				owner: (req param: 'id') asNumber
				fields: payload)) await.
			cid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ HttpResponse created: (Dictionary new at: 'id' put: cid; yourself) ] ].

		"GET /cards/:id -> 200 card data (this counts as a view)"
		aServer get: '/cards/:id' do: [:req |
			| pid |
			pid := (directory request: (LookupCard id: (req param: 'id') asNumber)) await.
			pid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #view) await) ] ].

		"GET /cards/:id/views -> 200 {views} (does NOT count)"
		aServer get: '/cards/:id/views' do: [:req |
			| pid |
			pid := (directory request: (LookupCard id: (req param: 'id') asNumber)) await.
			pid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #stats) await) ] ].

		"GET /health -> 200 {status:ok}"
		aServer get: '/health' do: [:req |
			(HttpResponse ok) json: (Dictionary new at: 'status' put: 'ok'; yourself) ].
		^aServer
	]
]


"---- Demo driver -----------------------------------------------------------"
[
	| addr port directory server client r aliceId bobId cardId
	  burstClients burstEach start elapsed collected done finalViews |

	addr := InternetAddress lookup: '127.0.0.1'.
	port := 8080.

	directory := Actors spawn: (Props fromActorClass: Directory).
	server := HttpServer on: addr port: port.
	BusinessCardApi on: server directory: directory.
	server start.
	Transcript nextPutAll: 'business-card API listening on 127.0.0.1:';
		nextPutAll: port printString; lf; lf.

	"one keep-alive client drives the whole demo"
	client := HttpClient on: addr port: port.

	"1) register two users"
	r := client post: '/users' json: (Dictionary new
		at: 'name' put: 'Alice'; at: 'email' put: 'alice@example.com'; yourself).
	aliceId := r jsonBody at: 'id'.
	r := client post: '/users' json: (Dictionary new
		at: 'name' put: 'Bob'; at: 'email' put: 'bob@example.com'; yourself).
	bobId := r jsonBody at: 'id'.
	Transcript nextPutAll: 'registered Alice (id '; nextPutAll: aliceId printString;
		nextPutAll: ') and Bob (id '; nextPutAll: bobId printString; nextPutAll: ')'; lf.

	"2) Alice creates a business card"
	r := client post: '/users/', aliceId printString, '/cards' json: (Dictionary new
		at: 'title' put: 'Founder';
		at: 'company' put: 'Smalltalk Works';
		at: 'phone' put: '+1-555-0100';
		yourself).
	cardId := r jsonBody at: 'id'.
	Transcript nextPutAll: 'Alice created card id '; nextPutAll: cardId printString; lf; lf.

	"3) read the card a few times; watch the view counter climb (actor state)"
	Transcript nextPutAll: 'reading GET /cards/'; nextPutAll: cardId printString;
		nextPutAll: ' three times:'; lf.
	1 to: 3 do: [:i |
		r := client get: '/cards/', cardId printString.
		Transcript nextPutAll: '   -> '; nextPutAll: r body; lf ].
	Transcript lf.

	"4) fetch the owner profile (shows the card id linked back to the user)"
	r := client get: '/users/', aliceId printString.
	Transcript nextPutAll: 'GET /users/'; nextPutAll: aliceId printString;
		nextPutAll: ' -> '; nextPutAll: r body; lf; lf.

	"5) concurrency burst: many keep-alive clients hammering the same card.
	    Every request funnels through the one card actor, so its counter stays
	    exact with no locks. Crank these numbers up to stress it harder."
	burstClients := 100.
	burstEach := 50.
	done := Channel new.
	start := DateTime currentMicroTime.
	1 to: burstClients do: [:ci |
		[
			| lc path |
			lc := HttpClient on: addr port: port.
			path := '/cards/', cardId printString.
			1 to: burstEach do: [:k | lc get: path ].
			lc close.
			done send: burstEach
		] fork ].
	collected := 0.
	1 to: burstClients do: [:ci | collected := collected + done receive ].
	elapsed := DateTime currentMicroTime - start.
	elapsed = 0 ifTrue: [ elapsed := 1 ].
	Transcript nextPutAll: 'burst: '; nextPutAll: collected printString;
		nextPutAll: ' requests across '; nextPutAll: burstClients printString;
		nextPutAll: ' concurrent connections in '; nextPutAll: (elapsed // 1000) printString;
		nextPutAll: ' ms => '; nextPutAll: ((collected * 1000000) // elapsed) printString;
		nextPutAll: ' req/s'; lf.

	"6) confirm the counter equals every read so far (3 + burst)"
	r := client get: '/cards/', cardId printString, '/views'.
	finalViews := r jsonBody at: 'views'.
	Transcript nextPutAll: 'final view count for card '; nextPutAll: cardId printString;
		nextPutAll: ' = '; nextPutAll: finalViews printString;
		nextPutAll: ' (expected '; nextPutAll: (3 + collected) printString; nextPutAll: ')'; lf.

	client close.
	server stop.
	Transcript lf; nextPutAll: 'server stopped, exiting.'; lf.
]
