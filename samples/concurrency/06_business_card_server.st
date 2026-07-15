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


"---- Typed JSON payloads (DTOs), same as 05: responses are plain objects
     rendered by Json's reflective encoder, request bodies bind via jsonBodyAs:. --"
UserFields := Object [
	| name email |
	class name: aName email: anEmail [ | r | r := self new. r setName: aName email: anEmail. ^r ]
	setName: aName email: anEmail [ name := aName. email := anEmail. ^self ]
	name [ ^name ]
	email [ ^email ]
]

CardFields := Object [
	| title company phone |
	class title: t company: c phone: p [ | r | r := self new. r setTitle: t company: c phone: p. ^r ]
	setTitle: t company: c phone: p [ title := t. company := c. phone := p. ^self ]
	title [ ^title ]
	company [ ^company ]
	phone [ ^phone ]
]

UserProfile := Object [
	| id name email cards |
	class id: i name: n email: e cards: c [ | r | r := self new. r setId: i name: n email: e cards: c. ^r ]
	setId: i name: n email: e cards: c [ id := i. name := n. email := e. cards := c. ^self ]
]

CardView := Object [
	| id owner views title company phone |
	class id: i owner: o views: v fields: f [ | r | r := self new. r setId: i owner: o views: v fields: f. ^r ]
	setId: i owner: o views: v fields: f [
		id := i. owner := o. views := v.
		f isNil ifFalse: [ title := f title. company := f company. phone := f phone ].
		^self ]
]

CardStats := Object [
	| id owner views |
	class id: i owner: o views: v [ | r | r := self new. r setId: i owner: o views: v. ^r ]
	setId: i owner: o views: v [ id := i. owner := o. views := v. ^self ]
]

IdReply := Object [
	| id |
	class of: i [ | r | r := self new. r setId: i. ^r ]
	setId: i [ id := i. ^self ]
]

Health := Object [
	| status |
	class ok [ | r | r := self new. r setOk. ^r ]
	setOk [ status := 'ok'. ^self ]
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
		^UserProfile id: id name: name email: email cards: cardIds asArray
	]
]

BusinessCardActor := Actor [
	| id ownerId fields views |
	class id: anId owner: anOwner fields: aCardFields [
		^self new setId: anId owner: anOwner fields: aCardFields ]
	setId: anId owner: anOwner fields: aCardFields [
		id := anId. ownerId := anOwner. fields := aCardFields. views := 0. ^self ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #view ifTrue: [ views := views + 1. ^ctx respond: self dataWithViews ].
		m = #stats ifTrue: [ ^ctx respond: self stats ].
	]
	dataWithViews [
		^CardView id: id owner: ownerId views: views fields: fields
	]
	stats [
		^CardStats id: id owner: ownerId views: views
	]
]

"A plain id->Pid registry -- NOT an actor. Reads are direct O(1) dictionary hits on
 the caller's own fiber (no mailbox, no round-trip). Request fibers run in PARALLEL
 across the scheduler's workers (the default now), so the registries are thread-safe
 SharedDictionaries and the id counters are bumped under the global monitor. Only
 mutation of owned state stays an actor round-trip: BusinessCardActor>>#view."
CardDirectory := Object [
	| users cards userSeq cardSeq |

	class new [ ^super new init ]
	init [ users := SharedDictionary new. cards := SharedDictionary new. userSeq := 0. cardSeq := 0. ^self ]

	"reads -- direct, no actor hop (SharedDictionary makes at:ifAbsent: atomic)"
	lookupUser: anId [ ^users at: anId ifAbsent: [ nil ] ]
	lookupCard: anId [ ^cards at: anId ifAbsent: [ nil ] ]

	"atomic id generation (a bare `seq := seq + 1` races across workers)"
	nextUserId [ | id | Processor monitorEnter. userSeq := userSeq + 1. id := userSeq. Processor monitorExit. ^id ]
	nextCardId [ | id | Processor monitorEnter. cardSeq := cardSeq + 1. id := cardSeq. Processor monitorExit. ^id ]

	"writes -- atomic id + concurrent-safe store; the spawn runs between them (no lock held)"
	registerUserName: aName email: anEmail [
		| uid pid |
		uid := self nextUserId.
		pid := Actors spawn: (Props fromProducer: [ UserActor id: uid name: aName email: anEmail ]).
		users at: uid put: pid.
		^uid ]

	createCardOwner: ownerId fields: aCardFields [
		| ownerPid cid cardPid |
		ownerPid := users at: ownerId ifAbsent: [ nil ].
		ownerPid isNil ifTrue: [ ^nil ].
		cid := self nextCardId.
		cardPid := Actors spawn: (Props fromProducer: [ BusinessCardActor id: cid owner: ownerId fields: aCardFields ]).
		cards at: cid put: cardPid.
		ownerPid tell: (AddCard id: cid).
		^cid ]
]


"---- Routes -----------------------------------------------------------------"
BusinessCardApi := Object [
	class on: aServer directory: directory [
		aServer post: '/users' do: [:req |
			| payload uid |
			payload := req jsonBodyAs: UserFields.
			(payload isNil or: [ payload name isNil ])
				ifTrue: [ HttpResponse badRequest: 'name is required' ]
				ifFalse: [
					uid := directory registerUserName: payload name
						email: (payload email isNil ifTrue: [ '' ] ifFalse: [ payload email ]).
					HttpResponse created: (IdReply of: uid) ] ].

		aServer get: '/users/:id' do: [:req |
			| pid |
			pid := directory lookupUser: (req param: 'id') asNumber.
			pid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #profile) await) ] ].

		aServer post: '/users/:id/cards' do: [:req |
			| payload cid |
			payload := req jsonBodyAs: CardFields. payload isNil ifTrue: [ payload := CardFields new ].
			cid := directory createCardOwner: (req param: 'id') asNumber fields: payload.
			cid isNil ifTrue: [ HttpResponse notFound ]
				ifFalse: [ HttpResponse created: (IdReply of: cid) ] ].

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
			(HttpResponse ok) json: Health ok ].

		"---- diagnostic endpoints (layer bisection for the req/s investigation) ----"
		"HTTP + exactly ONE actor round-trip + a tiny JSON payload.
		 Pings the seeded card actor via #stats (which does NOT count a view)."
		aServer get: '/actor-ping' do: [:req |
			(HttpResponse ok) json: (((directory lookupCard: 1) request: #stats) await) ].

		"HTTP + a card-shaped DTO build + REFLECTIVE JSON encode, but NO actor
		 round-trip. Isolates the JSON/allocation cost from the actor-message cost."
		aServer get: '/json-heavy' do: [:req |
			(HttpResponse ok) json: (CardView id: 1 owner: 1 views: 12345
				fields: (CardFields title: 'Founder' company: 'Smalltalk Works' phone: '+1-555-0100')) ].

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
	directory createCardOwner: 1 fields: (CardFields
		title: 'Founder' company: 'Smalltalk Works' phone: '+1-555-0100').

	Transcript nextPutAll: 'business-card API on http://0.0.0.0:'; nextPutAll: port printString; lf.
	Transcript nextPutAll: 'seeded: GET /cards/1 is ready. Ctrl-C to stop.'; lf.

	"park the main fiber forever so the accept loop keeps serving"
	[ true ] whileTrue: [ (Delay forSeconds: 3600) wait ].
]
