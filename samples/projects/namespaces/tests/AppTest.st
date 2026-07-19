[ | t found |
	t := TestRun named: 'NsShowcase'.
	t check: 'declared namespaces are reachable as globals'
		equals: 'hello, ns' is: [ (Greetings at: #Greeter) greet: 'ns' ].
	t check: 'arithmetic module' equals: 5 is: [ (Numbers at: #Stats) mean: #(2 4 6 8) ].
	t check: 'a namespace body can extend a core class'
		equals: 'HI!' is: [ 'hi' shout ].
	t check: 'same class name in two namespaces stays independent'
		that: [ (Banner at: #Formatter) ~~ (Plain at: #Formatter) ].
	t check: 'each keeps its own behavior'
		equals: '== x ==' is: [ (Banner at: #Formatter) format: 'x' ].

	"the registry is reflective: find our namespaces and their classes"
	found := OrderedCollection new.
	Namespaces keysDo: [ :name | found add: name ].
	t check: 'the registry lists the declared namespaces'
		that: [ (found includes: #Banner) and: [ (found includes: #Plain)
			and: [ found includes: #NsShowcase ] ] ].
	t check: 'namespace bindings enumerate the classes'
		that: [ | classes |
			classes := OrderedCollection new.
			(Namespaces at: #Numbers) bindings associationsDo: [ :assoc |
				(assoc value isKindOf: Class) ifTrue: [classes add: assoc key] ].
			classes includes: #Stats ].
	t check: 'method dictionaries enumerate the selectors'
		that: [ | sels |
			sels := OrderedCollection new.
			(Numbers at: #Stats) class methodDictionary keysDo: [ :sel | sels add: sel ].
			(sels includes: #mean:) and: [ sels includes: #sum: ] ].
	^t report
]
