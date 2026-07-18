BehaviorFooClass := Object [

	foo [

	]

]

[
	BehRun := TestRun named: 'Behavior'.

	BehRun check: 'superClass is Object' that: [BehaviorFooClass superClass == Object].
	BehRun check: 'Object subClasses includes it' that: [Object subClasses includes: BehaviorFooClass].
	BehRun check: 'methodDictionary size' equals: 1 is: [BehaviorFooClass methodDictionary size].
	BehRun check: 'methodDictionary includesKey foo' that: [BehaviorFooClass methodDictionary includesKey: #foo].
	BehRun check: 'inheritsFrom Object' that: [BehaviorFooClass inheritsFrom: Object].
	BehRun check: 'not inheritsFrom UndefinedObject' that: [(BehaviorFooClass inheritsFrom: UndefinedObject) = false].
	BehRun check: 'selectors includes foo' that: [BehaviorFooClass selectors includes: #foo].
	BehRun check: 'allSelectors includes foo' that: [BehaviorFooClass allSelectors includes: #foo].
	BehRun check: 'allSelectors includes isNil' that: [BehaviorFooClass allSelectors includes: #isNil].
	BehRun check: 'compiledMethodAt foo notNil' that: [(BehaviorFooClass compiledMethodAt: #foo) notNil].

	BehRun expect: 'Object new: raises' raises: Error in: [Object new: Object].
]


[ ^BehRun report ]
