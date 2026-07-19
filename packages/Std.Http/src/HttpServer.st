"A non-blocking HTTP/1.1 server. Because the VM's sockets park the calling fiber
 on the scheduler's epoll loop, a linear accept/serve loop actually multiplexes
 many connections on a single OS thread. Each accepted connection is served by
 its own fiber, and each connection is kept alive across requests by default, so
 the number of live fibers tracks the number of concurrent *connections*, not the
 number of requests.

     server := HttpServer port: 8080.
     server get: '/health' do: [:req | (HttpResponse ok) json: ...].
     server start.

 start returns as soon as the listening socket is bound (so clients may connect
 immediately); the accept loop runs in a forked fiber. stop unblocks the loop and
 releases the listener so a self-contained program can exit cleanly."
HttpServer := Object [

	| address port router listener running |


	class port: aPort [
		^self on: (InternetAddress lookup: '0.0.0.0') port: aPort
	]


	class on: anAddress port: aPort [
		^self new setAddress: anAddress port: aPort
	]


	setAddress: anAddress port: aPort [
		address := anAddress.
		port := aPort.
		router := HttpRouter new.
		running := false.
		^self
	]


	port [ ^port ]
	router [ ^router ]


	"---- routing (delegates to the router) ----"

	get: aPath do: aBlock [ router get: aPath do: aBlock. ^self ]
	post: aPath do: aBlock [ router post: aPath do: aBlock. ^self ]
	put: aPath do: aBlock [ router put: aPath do: aBlock. ^self ]
	delete: aPath do: aBlock [ router delete: aPath do: aBlock. ^self ]


	"---- lifecycle ----"

	start [
		listener := ServerSocket bindTo: address port: port queueSize: 1024.
		running := true.
		[
			[ running ] whileTrue: [
				| client |
				client := listener accept.
				running
					ifTrue: [ [ self serve: client ] fork ]
					ifFalse: [ client close ] ].
			listener close
		] fork.
		^self
	]


	stop [
		"Stop accepting and let the accept loop finish. We wake the loop, which is
		 parked in accept, by opening one throwaway connection to ourselves."
		running := false.
		[ (Socket connectTo: address port: port) close ] on: Error do: [:e | ].
		^self
	]


	"---- per-connection handling ----"

	serve: aSocket [
		| conn keepGoing |
		conn := HttpConnection on: aSocket.
		keepGoing := true.
		[
			[ keepGoing ] whileTrue: [
				| request response alive |
				request := conn readRequest.
				request isNil
					ifTrue: [ keepGoing := false ]
					ifFalse: [
						alive := request isKeepAlive.
						response := [ router dispatch: request ]
							on: Error
							do: [:e | HttpResponse serverError: e messageText ].
						response writeOn: aSocket keepAlive: alive.
						keepGoing := alive ] ]
		] ensure: [ aSocket close ].
		^self
	]

]
