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
		| reqSegs pathMatched bindings |
		reqSegs := aRequest pathSegments.
		pathMatched := false.
		routes do: [:route |
			bindings := self match: (route at: 2) against: reqSegs.
			bindings notNil ifTrue: [
				pathMatched := true.
				(route at: 1) = aRequest method ifTrue: [
					bindings keysAndValuesDo: [:k :v | aRequest param: k put: v ].
					^(route at: 3) value: aRequest ] ] ].
		pathMatched ifTrue: [ ^HttpResponse methodNotAllowed ].
		^HttpResponse notFound
	]


	match: routeSegs against: reqSegs [
		"Answer a Dictionary of bound params if the patterns match, else nil."
		| bindings |
		routeSegs size = reqSegs size ifFalse: [ ^nil ].
		bindings := Dictionary new.
		1 to: routeSegs size do: [:i |
			| rs qs |
			rs := routeSegs at: i.
			qs := reqSegs at: i.
			(rs size > 0 and: [ (rs at: 1) = $: ])
				ifTrue: [ bindings at: (rs copyFrom: 2 to: rs size) put: qs ]
				ifFalse: [ rs = qs ifFalse: [ ^nil ] ] ].
		^bindings
	]

]
