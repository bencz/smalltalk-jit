"Loads a project's package graph into the image. Reads package.st manifests
 (Compiler evaluate:, must answer a PackageSpec), resolves requires-by-name
 against the package roots, walks dependencies depth-first keyed by package
 name (cycle and conflicting-source detection), then loads each package in
 topological order: create its Namespace (imports = DIRECT requirements
 only), compile the manifest's files into it, send initialize to the new
 classes in definition order, and fail on unresolved names (vivified forward
 references that never got a definition). Load order within a package is the
 explicit files: list, superclass before subclass, same practice as the
 kernel bootstrap."
PackageLoader := Object [

	| specs loadOrder LoadedPackages |


	class initialize [
		LoadedPackages := Dictionary new.
	]


	class loadedPackages [
		^LoadedPackages
	]


	"loading"

	load: aProjectDir [
		"Resolve and load the graph rooted at aProjectDir/package.st; answer
		 the root PackageSpec."
		| rootSpec |
		specs := Dictionary new.
		loadOrder := OrderedCollection new.
		rootSpec := self specFrom: (File realpath: aProjectDir).
		specs at: rootSpec name put: rootSpec.
		self resolve: rootSpec stack: OrderedCollection new.
		loadOrder do: [ :spec | self loadPackage: spec ].
		^rootSpec
	]


	loadOrder [
		^loadOrder
	]


	specFrom: aPackageDir [
		| manifestPath source result |
		manifestPath := Path join: aPackageDir with: 'package.st'.
		(File exists: manifestPath) ifFalse: [Error signal: 'no package.st in ', aPackageDir].
		source := File readAll: manifestPath.
		result := Compiler new evaluate: source.
		(result isKindOf: PackageSpec) ifFalse: [
			Error signal: manifestPath, ' must answer a PackageSpec (end the expression with yourself)'].
		result rootPath: aPackageDir.
		result validateFrom: manifestPath.
		^result
	]


	"name resolution"

	packageRoots [
		"Search roots for requires-by-name, in precedence order: the
		 ST_PACKAGE_PATH entries, the user store, then the dist store next to
		 the executable (packages/ beside it, then ../packages for dev trees)."
		| roots env home exe dir |
		roots := OrderedCollection new.
		env := System env: 'ST_PACKAGE_PATH' ifAbsent: [nil].
		env notNil ifTrue: [
			(env splitBy: (Character value: 58)) do: [ :p |
				p isEmpty ifFalse: [roots add: p]]].
		home := System env: 'HOME' ifAbsent: [nil].
		home notNil ifTrue: [roots add: (Path join: (Path join: home with: '.st') with: 'packages')].
		exe := System executablePath.
		exe notNil ifTrue: [
			dir := Path dirname: exe.
			roots add: (Path join: dir with: 'packages').
			roots add: (Path join: (Path dirname: dir) with: 'packages')].
		^roots
	]


	resolveRequirement: aRequirement from: aSpec [
		"Answer the absolute directory of the required package."
		| candidate searched |
		aRequirement path notNil ifTrue: [
			candidate := (Path isAbsolute: aRequirement path)
				ifTrue: [aRequirement path]
				ifFalse: [Path join: aSpec rootPath with: aRequirement path].
			^File realpath: candidate].
		searched := OrderedCollection new.
		self packageRoots do: [ :root |
			candidate := Path join: root with: aRequirement name.
			searched add: candidate.
			(File exists: (Path join: candidate with: 'package.st')) ifTrue: [
				^File realpath: candidate]].
		Error signal: 'package ', aRequirement name, ' not found; searched: ', searched printString
	]


	resolve: aSpec stack: aStack [
		(aStack includes: aSpec name) ifTrue: [
			Error signal: 'dependency cycle: ', (self cycle: aStack closing: aSpec name)].
		aStack addLast: aSpec name.
		aSpec requirements do: [ :req |
			| existing dir child |
			existing := specs at: req name ifAbsent: [nil].
			existing isNil
				ifTrue: [
					dir := self resolveRequirement: req from: aSpec.
					child := self specFrom: dir.
					child name = req name ifFalse: [
						Error signal: dir, '/package.st declares name ', child name,
							' but was required as ', req name].
					self check: req against: child.
					specs at: req name put: child.
					self resolve: child stack: aStack]
				ifFalse: [
					req path notNil ifTrue: [
						dir := self resolveRequirement: req from: aSpec.
						dir = existing rootPath ifFalse: [
							Error signal: 'conflicting sources for package ', req name, ': ',
								existing rootPath, ' vs ', dir]].
					self check: req against: existing]].
		aStack removeLast.
		loadOrder addLast: aSpec.
	]


	cycle: aStack closing: aName [
		| s |
		s := ''.
		aStack do: [ :n | s := s, n, ' -> '].
		^s, aName
	]


	check: aRequirement against: aSpec [
		aRequirement version isNil ifTrue: [^self].
		aRequirement version = aSpec version ifFalse: [
			Error signal: 'package ', aSpec name, ' is version ', aSpec version printString,
				' but ', aRequirement version, ' was required (exact match in v1)'].
	]


	"package loading"

	loadPackage: aSpec [
		| nsName ns imports classes compiler |
		(LoadedPackages includesKey: aSpec name) ifTrue: [^self].
		nsName := aSpec name asSymbol.
		imports := OrderedCollection new.
		aSpec requirements do: [ :req | imports add: (Namespaces at: req name asSymbol)].
		ns := Namespace named: nsName imports: imports asArray.
		Namespaces at: nsName put: ns.
		compiler := Compiler new.
		classes := OrderedCollection new.
		aSpec files do: [ :f |
			classes addAll: (self compile: (Path join: aSpec rootPath with: f) into: ns with: compiler)].
		classes do: [ :c | c initialize].
		self reportUnresolved: ns spec: aSpec.
		LoadedPackages at: aSpec name put: aSpec.
	]


	compile: aPath into: aNamespace with: aCompiler [
		| stream parser classes node built |
		(File exists: aPath) ifFalse: [Error signal: 'source file missing: ', aPath].
		stream := FileStream read: aPath.
		parser := Parser parseStream: stream.
		classes := OrderedCollection new.
		[parser atEnd] whileFalse: [
			node := parser parseClass.
			built := aCompiler buildClass: node in: aNamespace.
			"extensions add methods to an EXISTING class and namespace
			 declarations initialize their own members: neither re-initializes"
			(node isExtension or: [node isNamespace]) ifFalse: [classes add: built]].
		stream close.
		^classes
	]


	reportUnresolved: aNamespace spec: aSpec [
		| allowed pending |
		allowed := aSpec allowUnresolved.
		pending := aNamespace unresolvedNames reject: [ :n | allowed includes: n].
		pending isEmpty ifFalse: [
			Error signal: 'package ', aSpec name, ' has unresolved names: ', pending printString,
				' (missing definition or typo; allowUnresolved: in package.st overrides)'].
	]

]
