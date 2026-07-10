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

"---- The only command message still sent to an actor: tell a UserActor it now
     owns a card. Resolution/registration are plain method calls on CardDirectory. --"
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

"A plain id->Pid registry -- NOT an actor. Reads are direct O(1) dictionary hits on
 the caller's own fiber (no mailbox, no round-trip); writes are synchronous and thus
 atomic under the cooperative single-OS-thread scheduler (no await/I/O/yield between the
 counter bump and the store, and spawn/tell don't yield the caller). Only mutation of
 owned state stays an actor round-trip: BusinessCardActor>>#view."
CardDirectory := Object [
	| users cards userSeq cardSeq |

	class new [ ^super new init ]
	init [ users := Dictionary new. cards := Dictionary new. userSeq := 0. cardSeq := 0. ^self ]

	"reads -- direct, no actor hop"
	lookupUser: anId [ ^users at: anId ifAbsent: [ nil ] ]
	lookupCard: anId [ ^cards at: anId ifAbsent: [ nil ] ]

	"writes -- synchronous, atomic under the cooperative scheduler"
	registerUserName: aName email: anEmail [
		| uid pid |
		userSeq := userSeq + 1. uid := userSeq.
		pid := Actors spawn: (Props fromProducer: [ UserActor id: uid name: aName email: anEmail ]).
		users at: uid put: pid.
		^uid ]

	createCardOwner: ownerId fields: aDict [
		| ownerPid cid cardPid |
		ownerPid := users at: ownerId ifAbsent: [ nil ].
		ownerPid isNil ifTrue: [ ^nil ].
		cardSeq := cardSeq + 1. cid := cardSeq.
		cardPid := Actors spawn: (Props fromProducer: [ BusinessCardActor id: cid owner: ownerId fields: aDict ]).
		cards at: cid put: cardPid.
		ownerPid tell: (AddCard id: cid).
		^cid ]
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
					uid := directory registerUserName: name email: (payload at: 'email' ifAbsent: [ '' ]).
					HttpResponse created: (Dictionary new at: 'id' put: uid; yourself) ] ].

		aServer get: '/users/:id' do: [:req |
			| pid |
			pid := directory lookupUser: (req param: 'id') asNumber.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #profile) await) ] ].

		aServer post: '/users/:id/cards' do: [:req |
			| payload cid |
			payload := req jsonBody. payload isNil ifTrue: [ payload := Dictionary new ].
			cid := directory createCardOwner: (req param: 'id') asNumber fields: payload.
			cid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ HttpResponse created: (Dictionary new at: 'id' put: cid; yourself) ] ].

		"Direct id->Pid lookup, then ONE round-trip to the card actor for #view."
		aServer get: '/cards/:id' do: [:req |
			| pid |
			pid := directory lookupCard: (req param: 'id') asNumber.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #view) await) ] ].

		aServer get: '/cards/:id/views' do: [:req |
			| pid |
			pid := directory lookupCard: (req param: 'id') asNumber.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #stats) await) ] ].

		aServer get: '/health' do: [:req |
			(HttpResponse ok) json: (Dictionary new at: 'status' put: 'ok'; yourself) ].

		"---- diagnostic endpoints (layer bisection for the req/s investigation) ----"
		"HTTP + exactly ONE actor round-trip + a tiny JSON payload.
		 Pings the seeded card actor via #stats (which does NOT count a view)."
		aServer get: '/actor-ping' do: [:req |
			(HttpResponse ok) json: (((directory lookupCard: 1) request: #stats) await) ].

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

		"Force a full GC and report the resulting live/scanned counts."
		aServer get: '/__gc' do: [:req |
			GarbageCollector collectGarbage.
			(HttpResponse ok) json: GarbageCollector lastStats ].

		^aServer
	]
]


"---- Boot the server and seed a demo user + card ---------------------------"
[
	| addr port directory server |
	addr := InternetAddress lookup: '0.0.0.0'.
	port := 8080.

	directory := CardDirectory new.
	server := HttpServer on: addr port: port.
	BusinessCardApi on: server directory: directory.
	server start.

	"seed: user 1 owns card 1"
	directory registerUserName: 'Alice' email: 'alice@example.com'.
	directory createCardOwner: 1 fields: (Dictionary new
		at: 'title' put: 'Founder';
		at: 'company' put: 'Smalltalk Works';
		at: 'phone' put: '+1-555-0100';
		yourself).

	Transcript nextPutAll: 'business-card API on http://0.0.0.0:'; nextPutAll: port printString; lf.
	Transcript nextPutAll: 'seeded: GET /cards/1 is ready. Ctrl-C to stop.'; lf.

	"park the main fiber forever so the accept loop keeps serving"
	[ true ] whileTrue: [ (Delay forSeconds: 3600) wait ].
]
