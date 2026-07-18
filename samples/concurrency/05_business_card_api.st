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
  and back, with handler fibers running IN PARALLEL across the
  scheduler's worker threads (one per core by default).

  All JSON payloads are typed DTOs: responses are plain objects
  rendered by Json's reflective encoder (instanceVariables +
  instVarAt:), and request bodies bind straight into DTO classes
  via jsonBodyAs: -- no hand-built Dictionaries on either side.

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

"---- The only command message still sent to an actor: tell a UserActor it now
     owns a card. Resolution/registration are NOT messages anymore -- the card
     directory below is a plain object, so those became direct method calls. ----"
AddCard := Object [
	| cardId |
	class id: anId [ ^self new setId: anId ]
	setId: anId [ cardId := anId. ^self ]
	cardId [ ^cardId ]
]


"---- Typed JSON payloads (DTOs). No hand-built Dictionaries anywhere: responses
     are plain objects rendered by Json's REFLECTIVE encoder (class
     instanceVariables + instVarAt: -> {'ivar': value, ...}), and request bodies
     are decoded straight into DTOs with jsonBodyAs:. The ivar names ARE the
     JSON keys, on both sides of the wire. ----"
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
	views [ ^views ]
]

IdReply := Object [
	| id |
	class of: i [ | r | r := self new. r setId: i. ^r ]
	setId: i [ id := i. ^self ]
	id [ ^id ]
]

Health := Object [
	| status |
	class ok [ | r | r := self new. r setOk. ^r ]
	setOk [ status := 'ok'. ^self ]
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
		^UserProfile id: id name: name email: email cards: cardIds asArray
	]
]


"---- Each business card is an actor that counts how often it is read ----"
BusinessCardActor := Actor [
	| id ownerId fields views |

	class id: anId owner: anOwner fields: aCardFields [
		^self new setId: anId owner: anOwner fields: aCardFields
	]

	setId: anId owner: anOwner fields: aCardFields [
		id := anId. ownerId := anOwner. fields := aCardFields. views := 0.
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
		^CardView id: id owner: ownerId views: views fields: fields
	]

	"just the counter, without counting this read"
	stats [
		^CardStats id: id owner: ownerId views: views
	]
]


"---- The card directory: a plain id->Pid registry. NOT an actor. ------------
     This is the whole point of the refactor. Resolving an id to a Pid is a read
     of near-immutable data (a card's Pid never changes once created), so it must
     NOT be an actor message on the hot path -- that would turn a lock-free lookup
     into a full round-trip and funnel every request through one mailbox.

       * reads (lookup*)  -> direct O(1) Dictionary hit on the caller's own fiber,
                             no mailbox, no round-trip. Exactly how the VM's own
                             `ActorSystem>>registry` (id->mailbox) is read on every tell:.
       * writes (register/create) -> plain synchronous methods. Under the cooperative
                             single-OS-thread scheduler they run atomically: there is
                             no await/I/O/yield between the counter bump and the store,
                             and `Actors spawn:`/`tell:` don't yield the caller, so two
                             concurrent POSTs cannot interleave and ids stay unique.
                             (Same guarantee the card actor gives `views`, obtained
                             structurally instead of via a mailbox.)

     Only mutation-of-owned-state stays an actor: BusinessCardActor>>#view. ----"
CardDirectory := Object [
	| users cards userSeq cardSeq |

	class new [ ^super new init ]

	init [
		"Shared registries read/written directly from many request fibers. Under the
		 multi-worker scheduler (the default now) those fibers run in PARALLEL, so the
		 registries must be thread-safe collections and the id counters must be bumped
		 atomically — the old 'atomic under the cooperative single-thread scheduler'
		 assumption no longer holds."
		users := SharedDictionary new.
		cards := SharedDictionary new.
		userSeq := AtomicInteger new.
		cardSeq := AtomicInteger new.
		^self
	]

	"reads -- direct, no actor hop (SharedDictionary makes at:ifAbsent: atomic)"
	lookupUser: anId [ ^users at: anId ifAbsent: [ nil ] ]
	lookupCard: anId [ ^cards at: anId ifAbsent: [ nil ] ]

	"atomic id generation: a lock-free fetch-add (AtomicInteger) instead of the global
	 sync monitor -- a bare `seq := seq + 1` races across workers into duplicate ids."
	nextUserId [ ^userSeq incrementAndGet ]
	nextCardId [ ^cardSeq incrementAndGet ]

	"writes -- atomic id + concurrent-safe store; the spawn runs between them (no lock held)"
	registerUserName: aName email: anEmail [
		| uid pid |
		uid := self nextUserId.
		pid := Actors spawn: (Props fromProducer: [
			UserActor id: uid name: aName email: anEmail ]).
		users at: uid put: pid.
		^uid
	]

	createCardOwner: ownerId fields: aDict [
		| ownerPid cid cardPid |
		ownerPid := users at: ownerId ifAbsent: [ nil ].
		ownerPid isNil ifTrue: [ ^nil ].
		cid := self nextCardId.
		cardPid := Actors spawn: (Props fromProducer: [
			BusinessCardActor id: cid owner: ownerId fields: aDict ]).
		cards at: cid put: cardPid.
		ownerPid tell: (AddCard id: cid).
		^cid
	]
]


"---- Wire the HTTP routes onto the actor system ----"
BusinessCardApi := Object [

	class on: aServer directory: directory [
		"POST /users {name,email} -> 201 {id}   (typed decode: body -> UserFields DTO)"
		aServer post: '/users' do: [:req |
			| payload uid |
			payload := req jsonBodyAs: UserFields.
			(payload isNil or: [ payload name isNil ])
				ifTrue: [ HttpResponse badRequest: 'name is required' ]
				ifFalse: [
					uid := directory
						registerUserName: payload name
						email: (payload email isNil ifTrue: [ '' ] ifFalse: [ payload email ]).
					HttpResponse created: (IdReply of: uid) ] ].

		"GET /users/:id -> 200 profile   (id->Pid is a direct lookup; the profile
		 is the only actor round-trip)"
		aServer get: '/users/:id' do: [:req |
			| pid |
			pid := directory lookupUser: (req param: 'id') asNumber.
			pid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #profile) await) ] ].

		"POST /users/:id/cards {title,company,phone} -> 201 {id}   (typed decode)"
		aServer post: '/users/:id/cards' do: [:req |
			| payload cid |
			payload := req jsonBodyAs: CardFields.
			payload isNil ifTrue: [ payload := CardFields new ].
			cid := directory
				createCardOwner: (req param: 'id') asNumber
				fields: payload.
			cid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ HttpResponse created: (IdReply of: cid) ] ].

		"GET /cards/:id -> 200 card data (this counts as a view).
		 Direct id->Pid lookup, then ONE round-trip to the card actor for #view."
		aServer get: '/cards/:id' do: [:req |
			| pid |
			pid := directory lookupCard: (req param: 'id') asNumber.
			pid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #view) await) ] ].

		"GET /cards/:id/views -> 200 {views} (does NOT count)"
		aServer get: '/cards/:id/views' do: [:req |
			| pid |
			pid := directory lookupCard: (req param: 'id') asNumber.
			pid isNil
				ifTrue: [ HttpResponse notFound ]
				ifFalse: [ (HttpResponse ok) json: ((pid request: #stats) await) ] ].

		"GET /health -> 200 {status:ok}"
		aServer get: '/health' do: [:req |
			(HttpResponse ok) json: Health ok ].
		^aServer
	]
]


"---- Demo driver -----------------------------------------------------------"
[
	| addr port directory server client r aliceId bobId cardId
	  burstClients burstEach start elapsed collected done finalViews |

	addr := InternetAddress lookup: '127.0.0.1'.
	port := 8080.

	directory := CardDirectory new.
	server := HttpServer on: addr port: port.
	BusinessCardApi on: server directory: directory.
	server start.
	Transcript nextPutAll: 'business-card API listening on 127.0.0.1:';
		nextPutAll: port printString; lf; lf.

	"one keep-alive client drives the whole demo"
	client := HttpClient on: addr port: port.

	"1) register two users -- the request DTOs are encoded reflexively too"
	r := client post: '/users' json: (UserFields name: 'Alice' email: 'alice@example.com').
	aliceId := (r jsonBodyAs: IdReply) id.
	r := client post: '/users' json: (UserFields name: 'Bob' email: 'bob@example.com').
	bobId := (r jsonBodyAs: IdReply) id.
	Transcript nextPutAll: 'registered Alice (id '; nextPutAll: aliceId printString;
		nextPutAll: ') and Bob (id '; nextPutAll: bobId printString; nextPutAll: ')'; lf.

	"2) Alice creates a business card"
	r := client post: '/users/', aliceId printString, '/cards' json: (CardFields
		title: 'Founder' company: 'Smalltalk Works' phone: '+1-555-0100').
	cardId := (r jsonBodyAs: IdReply) id.
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
	    The id->Pid lookup is now a direct dictionary read, so every request funnels
	    through the one card actor and NOTHING else -- its counter stays exact with no
	    locks, and there is no shared registry mailbox in the way. Crank these up."
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
	finalViews := (r jsonBodyAs: CardStats) views.
	Transcript nextPutAll: 'final view count for card '; nextPutAll: cardId printString;
		nextPutAll: ' = '; nextPutAll: finalViews printString;
		nextPutAll: ' (expected '; nextPutAll: (3 + collected) printString; nextPutAll: ')'; lf.

	client close.
	server stop.
	Transcript lf; nextPutAll: 'server stopped, exiting.'; lf.
]
