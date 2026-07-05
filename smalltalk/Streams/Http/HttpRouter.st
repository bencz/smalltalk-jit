"Maps a request's method + path pattern to a handler block. Patterns use ':name'
 segments as wildcards that bind a route param, e.g.

     router get: '/cards/:id' do: [:req | ... req param: 'id' ...].

 dispatch: answers the handler's HttpResponse, a 404 when no path matches, or a
 405 when the path matches but the method does not."
HttpRouter := Object [

	| routes |


	class new [
		^super new initRouter
	]


	initRouter [
		routes := OrderedCollection new.
		^self
	]


	"---- registration ----"

	on: aMethod path: aPath do: aBlock [
		| route |
		route := Array new: 3.
		route at: 1 put: aMethod asUppercase.
		route at: 2 put: (aPath splitBy: $/).
		route at: 3 put: aBlock.
		routes addLast: route.
		^self
	]


	get: aPath do: aBlock [ ^self on: 'GET' path: aPath do: aBlock ]
	post: aPath do: aBlock [ ^self on: 'POST' path: aPath do: aBlock ]
	put: aPath do: aBlock [ ^self on: 'PUT' path: aPath do: aBlock ]
	delete: aPath do: aBlock [ ^self on: 'DELETE' path: aPath do: aBlock ]


	"---- dispatch ----"

	dispatch: aRequest [
		| reqSegs pathMatched |
		reqSegs := aRequest pathSegments.
		pathMatched := false.
		routes do: [:route |
			(self segments: (route at: 2) match: reqSegs) ifTrue: [
				pathMatched := true.
				(route at: 1) = aRequest method ifTrue: [
					self bind: (route at: 2) from: reqSegs into: aRequest.
					^(route at: 3) value: aRequest ] ] ].
		pathMatched ifTrue: [ ^HttpResponse methodNotAllowed ].
		^HttpResponse notFound
	]


	segments: routeSegs match: reqSegs [
		"Does the route pattern match the request path? Allocation-free — a `:name`
		 segment matches anything, other segments must be equal."
		routeSegs size = reqSegs size ifFalse: [ ^false ].
		1 to: routeSegs size do: [:i |
			| rs |
			rs := routeSegs at: i.
			(rs size > 0 and: [ (rs at: 1) = $: ]) ifFalse: [
				rs = (reqSegs at: i) ifFalse: [ ^false ] ] ].
		^true
	]


	bind: routeSegs from: reqSegs into: aRequest [
		"Bind the :name params of the winning route into the request (only now,
		 so non-matching routes and param-less routes allocate nothing)."
		1 to: routeSegs size do: [:i |
			| rs |
			rs := routeSegs at: i.
			(rs size > 0 and: [ (rs at: 1) = $: ]) ifTrue: [
				aRequest param: (rs copyFrom: 2 to: rs size) put: (reqSegs at: i) ] ].
		^self
	]

]
