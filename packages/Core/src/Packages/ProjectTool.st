"CLI-facing driver behind the st project subcommands (build/run/test/new).
 The C side calls these through fixed eval strings and only stats files and
 writes the snapshot; everything project-shaped (manifest, graph, entry
 point, scaffolding) lives here. The entry record and the root package name
 are class variables, so they are IN the built project image: st run just
 loads the image and sends ProjectTool run, which is exactly what a future
 AOT executable will do after booting its embedded snapshot."
ProjectTool := Object [

	| EntryClassName EntrySelector EntryNamespace RootName |


	"shared"

	class findProjectRootFrom: aDir [
		| dir next |
		dir := File realpath: aDir.
		[true] whileTrue: [
			(File exists: (Path join: dir with: 'package.st')) ifTrue: [^dir].
			next := Path dirname: dir.
			next = dir ifTrue: [Error signal: 'no package.st found from ', aDir, ' upward'].
			dir := next].
	]


	class reportError: anException [
		| text |
		text := anException messageText.
		text isNil ifTrue: [text := anException class name].
		Transcript nextPutAll: 'error: '; nextPutAll: text; lf.
	]


	"st build: load the graph into this image, record the entry point, set
	 the session default namespace to the project, write build.deps, and
	 answer the output image path. The C caller then garbage-collects and
	 snapshots the image there. Answers nil after printing on failure."

	class build [
		^[self basicBuild] on: Error do: [ :e | self reportError: e. nil]
	]


	class basicBuild [
		| root loader spec |
		root := self findProjectRootFrom: System currentDirectory.
		loader := PackageLoader new.
		spec := loader load: root.
		RootName := spec name.
		EntryClassName := spec entryClassName.
		EntrySelector := spec entrySelector.
		EntryNamespace := spec name asSymbol.
		Namespace default: (Namespaces at: EntryNamespace).
		self writeDepsFor: loader root: root.
		^Path join: (Path join: root with: '.stbuild') with: 'program.img'
	]


	class writeDepsFor: aLoader root: aRoot [
		"One line per participating file, <mtimeMillis><TAB><absolutePath>:
		 every manifest and every compiled source of the resolved graph. The
		 C staleness probe stats each entry before deciding to rebuild."
		| dir lines |
		dir := Path join: aRoot with: '.stbuild'.
		(Directory exists: dir) ifFalse: [Directory create: dir].
		lines := ''.
		aLoader loadOrder do: [ :spec |
			lines := lines, (self depsLineFor: (Path join: spec rootPath with: 'package.st')).
			spec files do: [ :f |
				lines := lines, (self depsLineFor: (Path join: spec rootPath with: f))]].
		File writeAll: lines to: (Path join: dir with: 'build.deps').
	]


	class depsLineFor: aPath [
		^(File modificationTimeMillis: aPath) printString,
			(String with: Character tab), aPath, (String with: Character lf)
	]


	"st run: resolve the recorded entry point and invoke it with the
	 command-line arguments; the answer (a SmallInteger) is the exit code."

	class run [
		^[self basicRun] on: Error do: [ :e | self reportError: e. 1]
	]


	class basicRun [
		| ns cls result |
		EntryClassName isNil ifTrue: [
			Error signal: 'package ', (RootName isNil ifTrue: ['?'] ifFalse: [RootName]),
				' declares no entry point (add entry: #Main to package.st)'].
		ns := Namespaces at: EntryNamespace.
		cls := ns at: EntryClassName ifAbsent: [
			Error signal: 'entry class not found: ', EntryClassName].
		result := cls perform: EntrySelector with: System arguments.
		(result isKindOf: SmallInteger) ifTrue: [^result].
		^0
	]


	"st test: create <Root>Tests namespace importing the root package, make
	 it the session default, and answer the newline-joined test file paths;
	 the C side then runs each through the -f path (TestRun files answer
	 their fail count) and sums the exit codes. Answers nil on failure."

	class prepareTests [
		^[self basicPrepareTests] on: Error do: [ :e | self reportError: e. nil]
	]


	class basicPrepareTests [
		| root spec nsName imports testNs paths |
		root := self findProjectRootFrom: System currentDirectory.
		spec := PackageLoader loadedPackages at: RootName ifAbsent: [
			Error signal: 'the project image has no loaded root package (rebuild with st build)'].
		nsName := (RootName, 'Tests') asSymbol.
		"tests see the package under test AND its direct dependencies: a lib's
		 tests routinely exercise it against the things it builds on"
		imports := OrderedCollection new.
		imports add: (Namespaces at: RootName asSymbol).
		spec requirements do: [ :req | imports add: (Namespaces at: req name asSymbol)].
		testNs := Namespace named: nsName imports: imports asArray.
		Namespaces at: nsName put: testNs.
		Namespace default: testNs.
		paths := ''.
		spec testFiles do: [ :f |
			paths := paths, (Path join: root with: f), (String with: Character lf)].
		^paths
	]


	"st new <name>: scaffold a fresh application project."

	class scaffold [
		^[self basicScaffold] on: Error do: [ :e | self reportError: e. 1]
	]


	class basicScaffold [
		| args name |
		args := System arguments.
		args isEmpty ifTrue: [Error signal: 'usage: st new <name>'].
		name := args at: 1.
		(Directory exists: name) ifTrue: [Error signal: 'directory already exists: ', name].
		Directory create: name.
		Directory create: (Path join: name with: 'src').
		Directory create: (Path join: name with: 'tests').
		File writeAll: (self manifestTemplateFor: name) to: (Path join: name with: 'package.st').
		File writeAll: (self mainTemplateFor: name)
			to: (Path join: (Path join: name with: 'src') with: 'Main.st').
		File writeAll: (self testTemplateFor: name)
			to: (Path join: (Path join: name with: 'tests') with: 'MainTest.st').
		Transcript nextPutAll: 'created project '; nextPutAll: name; lf.
		^0
	]


	class nl [
		^String with: Character lf
	]


	class manifestTemplateFor: aName [
		^'"', aName, ' project manifest. Evaluated by the package loader;', self nl,
			' must answer a PackageSpec. Dependencies: requires: ''Std.Http'' resolves', self nl,
			' by name against the package roots; requires:path: is a local override."', self nl,
			'PackageSpec new', self nl,
			(String with: Character tab), 'name: ''', aName, ''';', self nl,
			(String with: Character tab), 'version: ''0.1.0'';', self nl,
			(String with: Character tab), 'files: #(''src/Main.st'');', self nl,
			(String with: Character tab), 'testFiles: #(''tests/MainTest.st'');', self nl,
			(String with: Character tab), 'entry: #Main;', self nl,
			(String with: Character tab), 'yourself', self nl
	]


	class mainTemplateFor: aName [
		^'Main := Object [', self nl, self nl,
			(String with: Character tab), 'class main: args [', self nl,
			(String with: Character tab), (String with: Character tab),
				'Transcript nextPutAll: ''hello from ', aName, '''; lf.', self nl,
			(String with: Character tab), (String with: Character tab), '^0', self nl,
			(String with: Character tab), ']', self nl, self nl,
			']', self nl
	]


	class testTemplateFor: aName [
		^'[ | t |', self nl,
			(String with: Character tab), 't := TestRun named: ''', aName, '''.', self nl,
			(String with: Character tab), 't check: ''truth holds'' that: [true].', self nl,
			(String with: Character tab), '^t report', self nl,
			']', self nl
	]

]
