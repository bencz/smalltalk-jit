Compiler := Object [

	includeFile: aString [
		| stream parser classes node built |
		stream := FileStream read: aString.
		parser := Parser parseStream: stream.
		classes := OrderedCollection new.
		[parser atEnd] whileFalse: [
			node := parser parseClass.
			built := self buildClass: node.
			"extensions and namespace declarations get no fresh initialize here"
			(node isExtension or: [node isNamespace]) ifFalse: [classes add: built]].
		classes do: [ :class | class initialize].
		stream close.
	]


	buildClass: aClassNode [
		| result |

		result := self basicBuildClass: aClassNode.
		(result isKindOf: Exception) ifTrue: [result signal].
		^result
	]


	basicBuildClass: aClassNode [
		<primitive: BuildClassPrimitive>
	]


	"namespace-aware builds: what the package loader uses. The plain
	 buildClass:/compileMethod:in: forms compile against the class home
	 namespace, falling back to Namespace default."

	buildClass: aClassNode in: aNamespace [
		| result |

		result := self basicBuildClass: aClassNode in: aNamespace.
		(result isKindOf: Exception) ifTrue: [result signal].
		^result
	]


	basicBuildClass: aClassNode in: aNamespace [
		<primitive: BuildClassInPrimitive>
	]


	compileMethod: aMethodNode in: aClass [
		| result |

		result := self basicCompileMethod: aMethodNode in: aClass.
		(result isKindOf: Exception) ifTrue: [result signal].
		^result
	]


	basicCompileMethod: aMethodNode in: aClass [
		<primitive: CompileMethodPrimitive>
	]


	compileMethod: aMethodNode in: aClass namespace: aNamespace [
		| result |

		result := self basicCompileMethod: aMethodNode in: aClass namespace: aNamespace.
		(result isKindOf: Exception) ifTrue: [result signal].
		^result
	]


	basicCompileMethod: aMethodNode in: aClass namespace: aNamespace [
		<primitive: CompileMethodInPrimitive>
	]


	evaluate: aString [
		| node method |

		node := (Parser parseString: 'eval [', aString, ']') parseMethod.
		node body expressions isEmpty ifFalse: [node body expressions last enableReturn].
		method := self compileMethod: node in: UndefinedObject namespace: Namespace default.
		^method sendTo: nil
	]


	evaluate: aString withArguments: aDictionary [
		| node method args i |

		node := (Parser parseString: 'eval [', aString, ']') parseMethod.
		node body expressions isEmpty ifFalse: [node body expressions last enableReturn].

		args := Array new: aDictionary size.
		i := 1.
		aDictionary keysAndValuesDo: [ :name :value |
			node body arguments add: (VariableNode value: name).
			args at: i put: value.
			i := i + 1].

		method := self compileMethod: node in: UndefinedObject namespace: Namespace default.
		^method sendTo: nil withArguments: args
	]

]
