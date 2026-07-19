[ | t |
	t := TestRun named: 'NsShowcase'.
	t check: 'module classes resolve through imports'
		equals: 'hello, ns' is: [ Greeter greet: 'ns' ].
	t check: 'arithmetic module' equals: 5 is: [ Stats mean: #(2 4 6 8) ].
	t check: 'a package extension of a core class is visible'
		equals: 'HI!' is: [ 'hi' shout ].
	t check: 'the first import wins on a shadowed name'
		equals: '== x ==' is: [ Formatter format: 'x' ].
	t check: 'the shadowed class stays reachable reflectively'
		equals: 'x' is: [ ((Namespaces at: #Plain) at: #Formatter) format: 'x' ].
	t check: 'same name, two namespaces, two classes'
		that: [ Formatter ~~ ((Namespaces at: #Plain) at: #Formatter) ].
	^t report
]
