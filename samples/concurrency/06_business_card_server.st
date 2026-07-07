"============================================================
  Concurrency 06 - Standalone business-card API server
  ------------------------------------------------------------
  The same actor-backed HTTP API as 05, but as a long-running
  server you can hammer with external tools. It binds 0.0.0.0:8080,
  seeds one demo user and one card (so GET /cards/1 works right
  away), and never shuts down.

  Run it:
     ./build/st -s snapshot -f samples/concurrency/06_business_card_server.st

  Then, from another shell:
     curl -s http://127.0.0.1:8080/cards/1        # watch views climb
     curl -s -XPOST http://127.0.0.1:8080/users -d {name:Bob,email:b@x.com}
     ab -k -n 50000 -c 200 http://127.0.0.1:8080/cards/1
     wrk -t4 -c200 -d10s http://127.0.0.1:8080/cards/1

  Endpoints (same as 05):
     POST /users                {name,email}   -> {id}
     GET  /users/:id                           -> profile + card ids
     POST /users/:id/cards      {title,...}     -> {id}
     GET  /cards/:id            (counts a view) -> card data + views
     GET  /cards/:id/views                      -> {views} (no count)
     GET  /health                               -> {status:ok}
  ============================================================"

"---- Command messages -------------------------------------------------------"
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


"---- Actors -----------------------------------------------------------------"
UserActor := Actor [
	| id name email cardIds |
	class id: anId name: aName email: anEmail [
		^self new setId: anId name: aName email: anEmail ]
	setId: anId name: aName email: anEmail [
		id := anId. name := aName. email := anEmail.
		cardIds := OrderedCollection new. ^self ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		(m isKindOf: AddCard) ifTrue: [ ^cardIds addLast: m cardId ].
		m = #profile ifTrue: [ ^ctx respond: self profile ].
	]
	profile [
		| d |
		d := Dictionary new: 8.
		d at: 'id' put: id. d at: 'name' put: name. d at: 'email' put: email.
		d at: 'cards' put: cardIds asArray.
		^d
	]
]

BusinessCardActor := Actor [
	| id ownerId fields views |
	class id: anId owner: anOwner fields: aDict [
		^self new setId: anId owner: anOwner fields: aDict ]
	setId: anId owner: anOwner fields: aDict [
		id := anId. ownerId := anOwner. fields := aDict. views := 0. ^self ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #view ifTrue: [ views := views + 1. ^ctx respond: self dataWithViews ].
		m = #stats ifTrue: [ ^ctx respond: self stats ].
	]
	dataWithViews [
		| d |
		d := Dictionary new: 8.
		fields keysAndValuesDo: [:k :v | d at: k put: v ].
		d at: 'id' put: id. d at: 'owner' put: ownerId. d at: 'views' put: views.
		^d
	]
	stats [
		| d |
		d := Dictionary new: 8.
		d at: 'id' put: id. d at: 'owner' put: ownerId. d at: 'views' put: views.
		^d
	]
]

Directory := Actor [
	| users cards userSeq cardSeq |
	started: ctx [ users := Dictionary new. cards := Dictionary new. userSeq := 0. cardSeq := 0 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m == #ping ifTrue: [ ^ctx respond: ((Dictionary new: 8) at: 'status' put: 'ok'; yourself) ].
		(m isKindOf: RegisterUser) ifTrue: [
			| uid pid |
			userSeq := userSeq + 1. uid := userSeq.
			pid := ctx spawn: (Props fromProducer: [ UserActor id: uid name: m name email: m email ]).
			users at: uid put: pid.
			^ctx respond: uid ].
		(m isKindOf: CreateCard) ifTrue: [
			| ownerPid cid cardPid |
			ownerPid := users at: m owner ifAbsent: [ nil ].
			ownerPid isNil ifTrue: [ ^ctx respond: nil ].
			cardSeq := cardSeq + 1. cid := cardSeq.
			cardPid := ctx spawn: (Props fromProducer: [ BusinessCardActor id: cid owner: m owner fields: m fields ]).
			cards at: cid put: cardPid.
			ownerPid tell: (AddCard id: cid).
			^ctx respond: cid ].
		(m isKindOf: LookupUser) ifTrue: [ ^ctx respond: (users at: m id ifAbsent: [ nil ]) ].
		(m isKindOf: LookupCard) ifTrue: [ ^ctx respond: (cards at: m id ifAbsent: [ nil ]) ].
	]
]


"---- Routes -----------------------------------------------------------------"
BusinessCardApi := Object [
	class on: aServer directory: directory [
		aServer post: '/users' do: [:req |
			| payload name uid |
			payload := req jsonBody.
			(payload isNil or: [ (payload at: 'name' ifAbsent: [ nil ]) isNil ])
				ifTrue: [ HttpResponse badRequest: 'name is required' ]
				ifFalse: [
					name := payload at: 'name'.
					uid := (directory request: (RegisterUser name: name email: (payload at: 'email' ifAbsent: [ '' ]))) await.
					HttpResponse created: (Dictionary new at: 'id' put: uid; yourself) ] ].

		aServer get: '/users/:id' do: [:req |
			| pid |
			pid := (directory request: (LookupUser id: (req param: 'id') asNumber)) await.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #profile) await) ] ].

		aServer post: '/users/:id/cards' do: [:req |
			| payload cid |
			payload := req jsonBody. payload isNil ifTrue: [ payload := Dictionary new ].
			cid := (directory request: (CreateCard owner: (req param: 'id') asNumber fields: payload)) await.
			cid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ HttpResponse created: (Dictionary new at: 'id' put: cid; yourself) ] ].

		aServer get: '/cards/:id' do: [:req |
			| pid |
			pid := (directory request: (LookupCard id: (req param: 'id') asNumber)) await.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #view) await) ] ].

		aServer get: '/cards/:id/views' do: [:req |
			| pid |
			pid := (directory request: (LookupCard id: (req param: 'id') asNumber)) await.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #stats) await) ] ].

		aServer get: '/health' do: [:req |
			(HttpResponse ok) json: (Dictionary new at: 'status' put: 'ok'; yourself) ].

		"---- diagnostic endpoints (layer bisection for the req/s investigation) ----"
		"HTTP + exactly ONE actor round-trip + a tiny JSON payload."
		aServer get: '/actor-ping' do: [:req |
			(HttpResponse ok) json: ((directory request: #ping) await) ].

		"HTTP + a card-shaped Dictionary build + JSON encode, but NO actor round-trip.
		 Isolates the JSON/Dictionary allocation cost from the actor-message cost."
		aServer get: '/json-heavy' do: [:req |
			| d |
			d := Dictionary new: 8.
			d at: 'title' put: 'Founder'.    d at: 'company' put: 'Smalltalk Works'.
			d at: 'phone' put: '+1-555-0100'. d at: 'id' put: 1.
			d at: 'owner' put: 1.             d at: 'views' put: 12345.
			(HttpResponse ok) json: d ].

		"VM GC/heap counters, sampled between stress waves."
		aServer get: '/__stats' do: [:req |
			(HttpResponse ok) json: GarbageCollector lastStats ].

		"Force a full GC and report the resulting live/scanned counts — used to tell
		 a genuine leak (marked stays huge) from floating garbage (marked small)."
		aServer get: '/__gc' do: [:req |
			GarbageCollector collectGarbage.
			(HttpResponse ok) json: GarbageCollector lastStats ].

		"Force a full GC then print a live-object-by-class census to stderr."
		aServer get: '/__histo' do: [:req |
			GarbageCollector collectGarbage.
			GarbageCollector printHistogram.
			(HttpResponse ok) json: GarbageCollector lastStats ].

		^aServer
	]
]


"---- Boot the server and seed a demo user + card ---------------------------"
[
	| addr port directory server |
	addr := InternetAddress lookup: '0.0.0.0'.
	port := 8080.

	directory := Actors spawn: (Props fromActorClass: Directory).
	server := HttpServer on: addr port: port.
	BusinessCardApi on: server directory: directory.
	server start.

	"seed: user 1 owns card 1"
	(directory request: (RegisterUser name: 'Alice' email: 'alice@example.com')) await.
	(directory request: (CreateCard owner: 1 fields: (Dictionary new
		at: 'title' put: 'Founder';
		at: 'company' put: 'Smalltalk Works';
		at: 'phone' put: '+1-555-0100';
		yourself))) await.

	Transcript nextPutAll: 'business-card API on http://0.0.0.0:'; nextPutAll: port printString; lf.
	Transcript nextPutAll: 'seeded: GET /cards/1 is ready. Ctrl-C to stop.'; lf.

	"park the main fiber forever so the accept loop keeps serving"
	[ true ] whileTrue: [ (Delay forSeconds: 3600) wait ].
]
