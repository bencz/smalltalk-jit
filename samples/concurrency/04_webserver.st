"============================================================
  Concurrency 04 - A concurrent HTTP server (and its clients)
  ------------------------------------------------------------
  Sockets in this VM are non-blocking: when an accept/read/write
  would block, the calling process parks on the scheduler's epoll
  loop instead of stalling the whole VM. So a server that reads
  linear, blocking-looking code actually multiplexes thousands of
  connections on a single OS thread (like Go or nginx).

  To stay self-contained this sample runs BOTH sides:
    * a server process that accepts N connections, forking one
      handler process per connection
    * N client processes that connect, send a request and read
      the status line back
  After N requests the server stops and the program exits.
  ============================================================"

[
	| crlf addr port ready listener requestCount |

	crlf := String new: 2.
	crlf at: 1 put: Character cr.
	crlf at: 2 put: Character lf.

	port := 8090.
	addr := InternetAddress lookup: '127.0.0.1'.
	ready := Semaphore new.
	requestCount := 5.

	"---- server process: accept, then one handler process per connection ----"
	[
		listener := ServerSocket bindTo: addr port: port queueSize: 16.
		Transcript nextPutAll: 'server: listening on 127.0.0.1:'.
		port printNl.
		ready signal.

		1 to: requestCount do: [:i |
			| client |
			client := listener accept.
			[
				| requestLine body response |
				requestLine := client upTo: Character lf.
				body := 'Hello from a Smalltalk fiber! (', requestLine, ')'.
				response := 'HTTP/1.1 200 OK' , crlf ,
					'Content-Type: text/plain' , crlf ,
					'Content-Length: ' , body size printString , crlf ,
					'Connection: close' , crlf , crlf , body.
				client nextPutAll: response.
				client close.
			] fork.
		].

		listener close.
		Transcript nextPutAll: 'server: handled all requests, shutting down.'; lf.
	] fork.

	"don't dial before the server is listening"
	ready wait.

	"---- client processes: connect, request, read the status line ----"
	1 to: requestCount do: [:i |
		| sock status |
		sock := Socket connectTo: addr port: port.
		sock nextPutAll: 'GET /req' , i printString , ' HTTP/1.1' , crlf ,
			'Host: localhost' , crlf , crlf.
		status := sock upTo: Character lf.
		Transcript nextPutAll: 'client '. i printNl.
		Transcript nextPutAll: '   <- '; nextPutAll: status; lf.
		sock close.
	].

	Transcript nextPutAll: 'all '; nextPutAll: requestCount printString;
		nextPutAll: ' requests completed.'; lf.
]
