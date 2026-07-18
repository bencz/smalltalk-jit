"Tests for the HTTP framework end-to-end: a forked HttpServer driven by an
 HttpClient over a real loopback socket. Exercises routing, path params, the
 query string, JSON request/response bodies, status codes and keep-alive.
 Self-contained and bounded so it exits cleanly under the test runner.
 Both blocks share one TestRun through the global HttpRun."

[
	| t addr port server client r |
	t := TestRun named: 'Http'.
	HttpRun := t.

	addr := InternetAddress lookup: '127.0.0.1'.
	port := 8130.

	server := HttpServer on: addr port: port.
	server get: '/health' do: [:req |
		(HttpResponse ok) json: (Dictionary new at: 'status' put: 'ok'; yourself) ].
	server get: '/cards/:id' do: [:req |
		(HttpResponse ok) json: (Dictionary new
			at: 'id' put: (req param: 'id');
			at: 'q' put: (req query: 'x');
			yourself) ].
	server post: '/echo' do: [:req | HttpResponse created: req jsonBody ].
	server start.

	client := HttpClient on: addr port: port.

	"GET returning JSON, status 200"
	r := client get: '/health'.
	t check: 'GET /health status' equals: 200 is: [ r status ].
	t check: 'GET /health json body' equals: 'ok' is: [ r jsonBody at: 'status' ].

	"route param + query string are parsed and exposed to the handler"
	r := client get: '/cards/42?x=hello'.
	t check: 'GET /cards/:id status' equals: 200 is: [ r status ].
	t check: 'route param parsed' equals: '42' is: [ r jsonBody at: 'id' ].
	t check: 'query string parsed' equals: 'hello' is: [ r jsonBody at: 'q' ].

	"POST with a JSON body, echoed back as 201 Created"
	r := client post: '/echo' json: (Dictionary new at: 'name' put: 'Bob'; yourself).
	t check: 'POST /echo status created' equals: 201 is: [ r status ].
	t check: 'POST /echo echoes json body' equals: 'Bob' is: [ r jsonBody at: 'name' ].

	"unknown path -> 404"
	t check: 'unknown path answers 404' equals: 404 is: [ (client get: '/nope') status ].

	"known path, unsupported method -> 405"
	t check: 'unsupported method answers 405' equals: 405 is: [ (client delete: '/health') status ].

	"keep-alive: many requests reuse the one connection"
	1 to: 10 do: [:i |
		t check: 'keep-alive request ', i printString equals: 200 is: [
			(client get: '/health') status ] ].

	client close.
	server stop.
]

"A second, concurrent client hitting the server proves fibers multiplex it."
[
	| t addr port server done total |
	t := HttpRun.
	addr := InternetAddress lookup: '127.0.0.1'.
	port := 8131.
	server := HttpServer on: addr port: port.
	server get: '/ping' do: [:req | (HttpResponse ok) text: 'pong' ].
	server start.

	done := Channel new.
	1 to: 8 do: [:c |
		[ | lc ok |
		  lc := HttpClient on: addr port: port.
		  ok := 0.
		  1 to: 10 do: [:i | (lc get: '/ping') status = 200 ifTrue: [ ok := ok + 1 ] ].
		  lc close.
		  done send: ok ] fork ].
	total := 0.
	1 to: 8 do: [:c | total := total + done receive ].
	t check: 'eight concurrent clients complete all requests' equals: 80 is: [ total ].

	server stop.
]

[
	^HttpRun report
]
