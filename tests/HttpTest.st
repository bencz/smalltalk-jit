"Tests for the HTTP framework end-to-end: a forked HttpServer driven by an
 HttpClient over a real loopback socket. Exercises routing, path params, the
 query string, JSON request/response bodies, status codes and keep-alive.
 Self-contained and bounded so it exits cleanly under the test runner."

[
	| addr port server client r |
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
	Assert true: r status = 200.
	Assert true: (r jsonBody at: 'status') = 'ok'.

	"route param + query string are parsed and exposed to the handler"
	r := client get: '/cards/42?x=hello'.
	Assert true: r status = 200.
	Assert true: (r jsonBody at: 'id') = '42'.
	Assert true: (r jsonBody at: 'q') = 'hello'.

	"POST with a JSON body, echoed back as 201 Created"
	r := client post: '/echo' json: (Dictionary new at: 'name' put: 'Bob'; yourself).
	Assert true: r status = 201.
	Assert true: (r jsonBody at: 'name') = 'Bob'.

	"unknown path -> 404"
	Assert true: (client get: '/nope') status = 404.

	"known path, unsupported method -> 405"
	Assert true: (client delete: '/health') status = 405.

	"keep-alive: many requests reuse the one connection"
	1 to: 10 do: [:i | Assert true: (client get: '/health') status = 200 ].

	client close.
	server stop.
]

"A second, concurrent client hitting the server proves fibers multiplex it."
[
	| addr port server done total |
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
	Assert true: total = 80.

	server stop.
]
