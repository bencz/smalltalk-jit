"============================================================================
  Concurrency 07 - An advanced Mediator (Command / Query / Event)
  ----------------------------------------------------------------------------
  The 'grown-up' version of samples/advanced/01_mediator.st. A Mediator is just
  a central dispatcher: components talk to IT, never to each other. Three kinds
  of message flow through it:

    * Command - changes state.  Goes to EXACTLY ONE handler, returns a result.
    * Query   - reads state.    Goes to EXACTLY ONE handler, returns a result.
    * Event   - a fact.         PUBLISHED fire-and-forget to ZERO..N subscribers;
                                the publisher does NOT wait for them. This is the
                                heart of the mediator/notification pattern.

  What makes this one 'advanced':

    - Every command/query runs through a PIPELINE of behaviors (middleware):
          [log]  ->  [validate]  ->  [time]  ->  handler
      exactly like MediatR pipeline behaviors. Validation can short-circuit and
      reject a bad command before any handler sees it.

    - Events are truly fire-and-forget: `publish:` spawns one FIBER per subscriber
      and returns immediately. Subscribers (receipt, analytics, audit, a reorder
      saga) then run concurrently, decoupled from the publisher and each other.

    - A subscriber may itself send a command: the reorder saga reacts to a
      `StockLow` event by issuing a `Restock` command. Events driving commands is
      how sagas / process managers are built.

    - The concurrency demo fires many commands as independent TASKS (fibers) and
      collects their results over a CHANNEL (fan-in). Because the scheduler runs
      each handler to completion between yields, plain in-memory state stays
      consistent - no locks needed.

  No actors here - just the Mediator, plain objects, fibers and a channel.

  Run it:
     ./build/st -s snapshot -f samples/concurrency/07_mediator_cqrs.st
  ============================================================================"


"===========================================================================
  THE MEDIATOR (generic - knows nothing about the domain)
  ==========================================================================="
Mediator := Object [
	| handlers subscribers behaviors |

	class new [ ^super new initMediator ]
	initMediator [
		handlers := Dictionary new.        "request class name -> the one handler block"
		subscribers := Dictionary new.     "event   class name -> subscriber blocks"
		behaviors := OrderedCollection new. "the pipeline wrapped around every handler"
		^self
	]

	"--- registration (fluent) ---"

	handle: aRequestClass with: aBlock [
		"Register the single handler for a command/query. aBlock is evaluated as
		 `value: request value: mediator`."
		handlers at: aRequestClass name put: aBlock.
		^self
	]

	use: aBehavior [
		"Add a pipeline behavior. First added = outermost wrapper."
		behaviors addLast: aBehavior.
		^self
	]

	subscribe: anEventClass with: aBlock [
		(subscribers at: anEventClass name ifAbsent: [ nil ]) isNil
			ifTrue: [ subscribers at: anEventClass name put: OrderedCollection new ].
		(subscribers at: anEventClass name) addLast: aBlock.
		^self
	]

	"--- command / query: routed to ONE handler, returns its result ---"

	send: aRequest [
		^self dispatch: aRequest
	]

	dispatch: aRequest [
		"Fold the behaviors around the terminal handler (innermost last), then run."
		| chain |
		chain := [:req | self runHandler: req ].
		behaviors reverseDo: [:b |
			| inner |
			inner := chain.
			chain := [:req | b handle: req next: inner ] ].
		^chain value: aRequest
	]

	runHandler: aRequest [
		| h |
		h := handlers at: aRequest class name ifAbsent: [ nil ].
		h isNil ifTrue: [ ^self error: 'no handler for ', aRequest class name ].
		^h value: aRequest value: self
	]

	"--- event: fire-and-forget to every subscriber ---"

	publish: anEvent [
		"Spawn one fiber per subscriber and return AT ONCE. We never wait for a
		 subscriber to finish - that is what makes an event fire-and-forget."
		| subs |
		subs := subscribers at: anEvent class name ifAbsent: [ nil ].
		subs isNil ifTrue: [ ^self ].
		subs do: [:handler | [ handler value: anEvent ] fork ].
		^self
	]
]


"A tiny shared, mutable flag the behaviors consult. Passing it as an object
 (not a bare global) keeps verbosity flips deterministic and lets the demo turn
 the pipeline chatter on for one command and off for the bulk runs."
TraceConfig := Object [
	| on |
	class new [ ^super new setOn: false ]
	setOn: aBool [ on := aBool. ^self ]
	on [ ^on ]
	on: aBool [ on := aBool. ^self ]
]

"--- Pipeline behaviors (middleware). Each wraps the next stage. ---
 Verbose lines are gated on a shared TraceConfig so bulk demos stay quiet."

LoggingBehavior := Object [
	| config |
	class on: aConfig [ ^self new setConfig: aConfig ]
	setConfig: aConfig [ config := aConfig. ^self ]
	handle: aRequest next: aBlock [
		config on ifTrue: [
			Transcript nextPutAll: '   |- [log]      '; nextPutAll: aRequest class name;
				nextPutAll: ': '; nextPutAll: aRequest describe; lf ].
		^aBlock value: aRequest
	]
]

ValidationBehavior := Object [
	| config |
	class on: aConfig [ ^self new setConfig: aConfig ]
	setConfig: aConfig [ config := aConfig. ^self ]
	handle: aRequest next: aBlock [
		"Ask the request to validate itself; short-circuit if invalid so the
		 handler never sees a bad command."
		| err |
		err := aRequest validate.
		err isNil ifFalse: [
			config on ifTrue: [
				Transcript nextPutAll: '   |- [validate] REJECT: '; nextPutAll: err; lf ].
			^Rejection reason: err ].
		^aBlock value: aRequest
	]
]

TimingBehavior := Object [
	| config |
	class on: aConfig [ ^self new setConfig: aConfig ]
	setConfig: aConfig [ config := aConfig. ^self ]
	handle: aRequest next: aBlock [
		| t0 result |
		t0 := DateTime currentMicroTime.
		result := aBlock value: aRequest.
		config on ifTrue: [
			Transcript nextPutAll: '   |- [time]     '; nextPutAll: aRequest class name;
				nextPutAll: ' handled in '; nextPutAll: (DateTime currentMicroTime - t0) printString;
				nextPutAll: ' us'; lf ].
		^result
	]
]

"What a request becomes when validation refuses it (polymorphic with OrderResult)."
Rejection := Object [
	| reason |
	class reason: aReason [ ^self new setReason: aReason ]
	setReason: aReason [ reason := aReason. ^self ]
	reason [ ^reason ]
	status [ ^#invalid ]
	describe [ ^'REJECTED by validation: ', reason ]
]


"===========================================================================
  DOMAIN - Requests (Commands & Queries), Events, results and plain state
  ==========================================================================="

"Base request: everything can validate (default: ok) and describe itself."
Request := Object [
	validate [ ^nil ]
	describe [ ^self class name ]
]

"Commands (change state)"
PlaceOrder := Request [
	| sku qty customer |
	class sku: s qty: q customer: c [ ^self new setSku: s qty: q customer: c ]
	setSku: s qty: q customer: c [ sku := s. qty := q. customer := c. ^self ]
	sku [ ^sku ]  qty [ ^qty ]  customer [ ^customer ]
	validate [ qty > 0 ifFalse: [ ^'quantity must be positive' ]. ^nil ]
	describe [ ^customer, ' buys ', qty printString, 'x ', sku ]
]

Restock := Request [
	| sku qty |
	class sku: s qty: q [ ^self new setSku: s qty: q ]
	setSku: s qty: q [ sku := s. qty := q. ^self ]
	sku [ ^sku ]  qty [ ^qty ]
	describe [ ^'restock ', qty printString, 'x ', sku ]
]

"Queries (read state)"
GetStock := Request [
	| sku |
	class sku: s [ ^self new setSku: s ]
	setSku: s [ sku := s. ^self ]
	sku [ ^sku ]
	describe [ ^'stock of ', sku ]
]

GetCatalog := Request [
	describe [ ^'full catalog' ]
]

"Events (facts, published fire-and-forget)"
Event := Object [
	describe [ ^self class name ]
]

OrderPlaced := Event [
	| orderId sku qty customer total remaining |
	class fromResult: r [ ^self new setFrom: r ]
	setFrom: r [
		orderId := r orderId. sku := r sku. qty := r qty.
		customer := r customer. total := r total. remaining := r remaining. ^self ]
	orderId [ ^orderId ]  sku [ ^sku ]  qty [ ^qty ]
	customer [ ^customer ]  total [ ^total ]  remaining [ ^remaining ]
	describe [
		^'OrderPlaced #', orderId printString, ' ', qty printString, 'x ', sku,
			' ($', total printString, ') for ', customer ]
]

OrderRejected := Event [
	| sku qty customer reason |
	class fromResult: r [ ^self new setFrom: r ]
	setFrom: r [ sku := r sku. qty := r qty. customer := r customer. reason := r reason. ^self ]
	sku [ ^sku ]  qty [ ^qty ]  customer [ ^customer ]  reason [ ^reason ]
	describe [
		^'OrderRejected ', qty printString, 'x ', sku, ' for ', customer, ' (', reason, ')' ]
]

StockLow := Event [
	| sku remaining |
	class sku: s remaining: r [ ^self new setSku: s remaining: r ]
	setSku: s remaining: r [ sku := s. remaining := r. ^self ]
	sku [ ^sku ]  remaining [ ^remaining ]
	describe [ ^'StockLow ', sku, ' (', remaining printString, ' left)' ]
]

"The value a PlaceOrder resolves to."
OrderResult := Object [
	| status orderId sku qty customer total remaining reason |
	class placed: anId for: aReq total: aTotal remaining: rem [
		^self new setStatus: #placed id: anId sku: aReq sku qty: aReq qty
			customer: aReq customer total: aTotal remaining: rem reason: nil ]
	class rejected: aReq reason: aReason remaining: rem [
		^self new setStatus: #rejected id: nil sku: aReq sku qty: aReq qty
			customer: aReq customer total: 0 remaining: rem reason: aReason ]
	setStatus: st id: anId sku: s qty: q customer: c total: t remaining: rem reason: rsn [
		status := st. orderId := anId. sku := s. qty := q.
		customer := c. total := t. remaining := rem. reason := rsn. ^self ]
	status [ ^status ]  orderId [ ^orderId ]  sku [ ^sku ]  qty [ ^qty ]
	customer [ ^customer ]  total [ ^total ]  remaining [ ^remaining ]  reason [ ^reason ]
	describe [
		status == #placed
			ifTrue: [ ^'PLACED #', orderId printString, ' (', qty printString, 'x ', sku,
				' = $', total printString, ', ', remaining printString, ' left)' ]
			ifFalse: [ ^'REJECTED (', reason, ') - ', customer, ' wanted ',
				qty printString, 'x ', sku ]
	]
]

"Plain domain state. No locking: the cooperative scheduler runs each command
 handler to completion between yields, so concurrent commands never interleave
 mid-mutation."
Store := Object [
	| stock prices seq |
	class new [ ^super new initStore ]
	initStore [
		stock := Dictionary new. prices := Dictionary new. seq := 0.
		stock at: 'ESPRESSO' put: 30.     prices at: 'ESPRESSO' put: 3.
		stock at: 'LATTE' put: 30.        prices at: 'LATTE' put: 4.
		stock at: 'LIMITED-MUG' put: 8.   prices at: 'LIMITED-MUG' put: 25.
		^self
	]

	stockOf: aSku [ ^stock at: aSku ifAbsent: [ 0 ] ]

	catalog [
		| d |
		d := Dictionary new.
		stock keysAndValuesDo: [:k :v | d at: k put: v ].
		^d
	]

	place: aReq [
		| have price |
		have := stock at: aReq sku ifAbsent: [
			^OrderResult rejected: aReq reason: 'unknown item' remaining: 0 ].
		have < aReq qty ifTrue: [
			^OrderResult rejected: aReq reason: 'insufficient stock' remaining: have ].
		price := prices at: aReq sku ifAbsent: [ 0 ].
		stock at: aReq sku put: have - aReq qty.
		seq := seq + 1.
		^OrderResult placed: seq for: aReq total: aReq qty * price remaining: have - aReq qty
	]

	restock: aReq [
		| have |
		have := stock at: aReq sku ifAbsent: [ 0 ].
		stock at: aReq sku put: have + aReq qty.
		^have + aReq qty
	]
]


"===========================================================================
  APPLICATION - wires handlers, behaviors and subscribers onto the mediator
  ==========================================================================="
ShopApplication := Object [
	| mediator store auditLog stats trace |

	class boot [ ^self new boot ]
	boot [
		mediator := Mediator new.
		store := Store new.
		auditLog := OrderedCollection new.
		trace := TraceConfig new.
		stats := Dictionary new.
		stats at: 'orders' put: 0. stats at: 'revenue' put: 0. stats at: 'rejected' put: 0.
		self registerBehaviors; registerHandlers; wireSubscribers.
		^self
	]

	"--- accessors & policy ---"
	mediator [ ^mediator ]
	auditLog [ ^auditLog ]
	stats [ ^stats ]
	trace [ ^trace ]
	lowStockThreshold [ ^5 ]
	restockAmount [ ^20 ]
	reorderable: aSku [ ^(aSku = 'LIMITED-MUG') not ]

	"--- pipeline: order matters. First used = outermost. ---"
	registerBehaviors [
		mediator use: (LoggingBehavior on: trace).
		mediator use: (ValidationBehavior on: trace).
		mediator use: (TimingBehavior on: trace).
		^self
	]

	"--- one handler per request type ---"
	registerHandlers [
		mediator handle: PlaceOrder with: [:req :med | self onPlaceOrder: req on: med ].
		mediator handle: Restock    with: [:req :med | store restock: req ].
		mediator handle: GetStock   with: [:req :med | store stockOf: req sku ].
		mediator handle: GetCatalog with: [:req :med | store catalog ].
		^self
	]

	"Command handler: mutate state, THEN publish events describing what happened."
	onPlaceOrder: aReq on: med [
		| r |
		r := store place: aReq.
		r status == #placed
			ifTrue: [
				med publish: (OrderPlaced fromResult: r).
				"fire StockLow only on the order that crosses the threshold downward"
				(r remaining <= self lowStockThreshold
					and: [ (r remaining + aReq qty) > self lowStockThreshold ])
					ifTrue: [ med publish: (StockLow sku: r sku remaining: r remaining) ] ]
			ifFalse: [ med publish: (OrderRejected fromResult: r) ].
		^r
	]

	"--- event subscribers: independent reactions to facts ---"
	wireSubscribers [
		mediator subscribe: OrderPlaced   with: [:ev | self printReceipt: ev ].
		mediator subscribe: OrderPlaced   with: [:ev | self recordPlaced: ev ].
		mediator subscribe: OrderPlaced   with: [:ev | auditLog addLast: ev describe ].
		mediator subscribe: OrderRejected with: [:ev | self recordRejected: ev ].
		mediator subscribe: OrderRejected with: [:ev | auditLog addLast: ev describe ].
		mediator subscribe: StockLow      with: [:ev | auditLog addLast: ev describe ].
		mediator subscribe: StockLow      with: [:ev | self onStockLow: ev ].
		^self
	]

	recordPlaced: ev [
		stats at: 'orders'  put: (stats at: 'orders') + 1.
		stats at: 'revenue' put: (stats at: 'revenue') + ev total.
		^self
	]

	recordRejected: ev [
		stats at: 'rejected' put: (stats at: 'rejected') + 1.
		^self
	]

	printReceipt: ev [
		trace on ifFalse: [ ^self ].
		Transcript nextPutAll: '      [receipt] '; nextPutAll: ev customer;
			nextPutAll: ' order #'; nextPutAll: ev orderId printString;
			nextPutAll: ': '; nextPutAll: ev qty printString; nextPutAll: 'x ';
			nextPutAll: ev sku; nextPutAll: ' = $'; nextPutAll: ev total printString; lf.
		^self
	]

	"the saga: an event handler that ISSUES A COMMAND."
	onStockLow: ev [
		(self reorderable: ev sku) ifFalse: [
			Transcript nextPutAll: '      [saga] '; nextPutAll: ev sku;
				nextPutAll: ' low ('; nextPutAll: ev remaining printString;
				nextPutAll: ' left) - not reorderable, skipping'; lf.
			^self ].
		Transcript nextPutAll: '      [saga] '; nextPutAll: ev sku;
			nextPutAll: ' low ('; nextPutAll: ev remaining printString;
			nextPutAll: ' left) -> auto-reordering +'; nextPutAll: self restockAmount printString; lf.
		mediator send: (Restock sku: ev sku qty: self restockAmount).
		^self
	]

	"--- helpers ---"
	printAuditTail: n [
		| start |
		start := (auditLog size - n) + 1.
		start < 1 ifTrue: [ start := 1 ].
		start to: auditLog size do: [:i |
			Transcript nextPutAll: '   * '; nextPutAll: (auditLog at: i); lf ].
		^self
	]

	"let the fire-and-forget subscriber fibers run before we read/print"
	drain [ 1 to: 30 do: [:i | Processor yield ]. ^self ]
]


"===========================================================================
  DEMO
  ==========================================================================="

"--- boot ---"
[
	App := ShopApplication boot.
	Transcript nextPutAll: '================================================================'; lf.
	Transcript nextPutAll: '  ADVANCED MEDIATOR - Command / Query / Event'; lf.
	Transcript nextPutAll: '================================================================'; lf.
	Transcript nextPutAll: 'Command/Query -> exactly one handler (returns a result).'; lf.
	Transcript nextPutAll: 'Event         -> fire-and-forget to every subscriber (its own fiber).'; lf.
	Transcript nextPutAll: 'Pipeline      -> [log] -> [validate] -> [time] -> handler.'; lf.
	Transcript lf.
]

"--- 1. anatomy of one command: full pipeline + event fan-out ---"
[
	| latte bad |
	App trace on: true.
	Transcript nextPutAll: '---- 1. anatomy of one command (pipeline + event fan-out) ----'; lf.
	Transcript nextPutAll: 'send PlaceOrder(Ana, 2x LATTE):'; lf.
	latte := App mediator send: (PlaceOrder sku: 'LATTE' qty: 2 customer: 'Ana').
	App drain.
	Transcript nextPutAll: '   => '; nextPutAll: latte describe; lf.
	Transcript lf.
	Transcript nextPutAll: 'send an INVALID PlaceOrder(qty 0) - blocked by [validate]:'; lf.
	bad := App mediator send: (PlaceOrder sku: 'LATTE' qty: 0 customer: 'Bob').
	Transcript nextPutAll: '   => '; nextPutAll: bad describe; lf.
	Transcript lf.
]

"--- 2. many commands as concurrent tasks; results collected over a channel ---"
[
	| results placed rejected |
	App trace on: false.
	results := Channel new.
	Transcript nextPutAll: '---- 2. flash sale: 12 concurrent buyers, 8 LIMITED-MUG in stock ----'; lf.
	Transcript nextPutAll: 'stock before: '.
	(App mediator send: (GetStock sku: 'LIMITED-MUG')) printNl.

	"fan-out: each buyer is its own fiber (a task) sending a command"
	1 to: 12 do: [:i |
		[ results send:
			(App mediator send: (PlaceOrder sku: 'LIMITED-MUG' qty: 1 customer: 'buyer')) ] fork ].

	"fan-in: pull the 12 results back off the channel"
	placed := 0. rejected := 0.
	1 to: 12 do: [:i |
		results receive status == #placed
			ifTrue: [ placed := placed + 1 ]
			ifFalse: [ rejected := rejected + 1 ] ].
	App drain.
	Transcript nextPutAll: 'placed='; nextPutAll: placed printString;
		nextPutAll: '  rejected='; nextPutAll: rejected printString; lf.
	Transcript nextPutAll: 'stock after:  '.
	(App mediator send: (GetStock sku: 'LIMITED-MUG')) printNl.
	Transcript nextPutAll: '(never oversold: handlers run atomically between yields)'; lf.
	Transcript lf.
]

"--- 3. event-driven saga: a StockLow event fires a Restock command ---"
[
	Transcript nextPutAll: '---- 3. saga: a StockLow event fires a Restock command ----'; lf.
	Transcript nextPutAll: 'ESPRESSO before: '.
	(App mediator send: (GetStock sku: 'ESPRESSO')) printNl.
	Transcript nextPutAll: 'buying 26 shots (threshold is '; nextPutAll: App lowStockThreshold printString;
		nextPutAll: ')...'; lf.
	1 to: 26 do: [:i | App mediator send: (PlaceOrder sku: 'ESPRESSO' qty: 1 customer: 'rush') ].
	App drain.
	Transcript nextPutAll: 'ESPRESSO after:  '.
	(App mediator send: (GetStock sku: 'ESPRESSO')) printNl.
	Transcript nextPutAll: '(30 - 26 sold + 20 auto-restock = 24, without the caller asking)'; lf.
	Transcript lf.
]

"--- 4. read model (analytics) + audit trail, both built from events ---"
[
	Transcript nextPutAll: '---- 4. read model + audit trail (both fed by fire-and-forget) ----'; lf.
	Transcript nextPutAll: 'analytics:  orders='; nextPutAll: (App stats at: 'orders') printString;
		nextPutAll: '  revenue=$'; nextPutAll: (App stats at: 'revenue') printString;
		nextPutAll: '  rejected='; nextPutAll: (App stats at: 'rejected') printString; lf.
	Transcript nextPutAll: 'audit captured '; nextPutAll: App auditLog size printString;
		nextPutAll: ' events; last few:'; lf.
	App printAuditTail: 6.
	Transcript lf.
	Transcript nextPutAll: 'Done. One mediator; commands/queries to one handler each;'; lf.
	Transcript nextPutAll: 'events fire-and-forget to many. No actors, no locks.'; lf.
]
