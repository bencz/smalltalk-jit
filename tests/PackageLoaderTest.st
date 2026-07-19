"PackageLoader: dependency graph resolution and package loading, exercised
 against fixture package trees written to a temp directory. All fixture
 requirements use requires:path: so the test is hermetic (no search roots
 involved); requires-by-name resolution is covered by the shell e2e gate in
 run_tests.sh. Fixture names are unique per concern because loaded packages
 register globally in this process."

PkgFix := Object [

	| Root |

	class setup [
		Root := '/tmp/st-pkgloader-', System monotonicNanos printString.
		Directory create: Root.
	]

	class root [
		^Root
	]

	class dir: aName manifest: aManifest [
		| d |
		d := Path join: Root with: aName.
		Directory create: d.
		File writeAll: aManifest to: (Path join: d with: 'package.st').
		^d
	]

	class dir: aName manifest: aManifest file: aFile source: aSource [
		| d |
		d := self dir: aName manifest: aManifest.
		Directory create: (Path join: d with: 'src').
		File writeAll: aSource to: (Path join: d with: aFile).
		^d
	]

	class teardown [
		"Best-effort recursive delete of the fixture tree (two levels deep)."
		(Directory directoriesIn: Root) do: [ :d |
			(Directory directoriesIn: d) do: [ :dd |
				(Directory filesIn: dd) do: [ :f | File delete: f ].
				Directory delete: dd].
			(Directory filesIn: d) do: [ :f | File delete: f ].
			Directory delete: d].
		(Directory filesIn: Root) do: [ :f | File delete: f ].
		Directory delete: Root.
	]

]

[ | t names dRoot |
	t := TestRun named: 'PackageLoader'.
	PkgFix setup.

	"diamond: Root -> B, C; B -> D; C -> D. D loads once, before B and C.
	 D's source file holds TWO classes: regression for the fclose-after-restore
	 bug that broke the second parseClass of any multi-class file."
	PkgFix dir: 'DiaD' manifest: 'PackageSpec new name: ''DiaD''; files: #(''src/D.st''); yourself'
		file: 'src/D.st' source: 'DiaDClass := Object [ tag [ ^#d ] ]

DiaDExtra := Object [ tag [ ^#extra ] ]'.
	PkgFix dir: 'DiaB' manifest: 'PackageSpec new name: ''DiaB''; requires: ''DiaD'' path: ''../DiaD''; files: #(''src/B.st''); yourself'
		file: 'src/B.st' source: 'DiaBClass := Object [ use [ ^DiaDClass new tag ] ]'.
	PkgFix dir: 'DiaC' manifest: 'PackageSpec new name: ''DiaC''; requires: ''DiaD'' path: ''../DiaD''; files: #(''src/C.st''); yourself'
		file: 'src/C.st' source: 'DiaCClass := Object [ use [ ^DiaDClass new tag ] ]'.
	dRoot := PkgFix dir: 'DiaRoot'
		manifest: 'PackageSpec new name: ''DiaRoot''; requires: ''DiaB'' path: ''../DiaB''; requires: ''DiaC'' path: ''../DiaC''; yourself'.

	names := nil.
	t check: 'a diamond graph loads' that: [ | loader |
		loader := PackageLoader new.
		loader load: dRoot.
		names := loader loadOrder collect: [ :s | s name ].
		true ].
	t check: 'the shared dependency appears once in the load order'
		equals: 4 is: [ names size ].
	t check: 'dependencies load before their dependents' that: [
		((names indexOf: 'DiaD') < (names indexOf: 'DiaB'))
			and: [ ((names indexOf: 'DiaD') < (names indexOf: 'DiaC'))
			and: [ (names indexOf: 'DiaRoot') = names size ] ] ].
	t check: 'each package got its namespace with its classes'
		that: [ ((Namespaces at: #DiaD) includesKey: #DiaDClass)
			and: [ (Namespaces at: #DiaB) includesKey: #DiaBClass ] ].
	t check: 'the SECOND class of a multi-class source file loaded too'
		equals: #extra is: [ ((Namespaces at: #DiaD) at: #DiaDExtra) new tag ].
	t check: 'imported classes resolved across package namespaces'
		equals: #d is: [ ((Namespaces at: #DiaB) at: #DiaBClass) new use ].
	t check: 'loaded packages are registered'
		that: [ PackageLoader loadedPackages includesKey: 'DiaD' ].

	"cycle: CycA -> CycB -> CycA"
	PkgFix dir: 'CycA' manifest: 'PackageSpec new name: ''CycA''; requires: ''CycB'' path: ''../CycB''; yourself'.
	PkgFix dir: 'CycB' manifest: 'PackageSpec new name: ''CycB''; requires: ''CycA'' path: ''../CycA''; yourself'.
	t expect: 'a dependency cycle is an error' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'CycA') ].

	"conflicting sources: two paths for one name"
	PkgFix dir: 'ConfX1' manifest: 'PackageSpec new name: ''ConfX''; yourself'.
	PkgFix dir: 'ConfX2' manifest: 'PackageSpec new name: ''ConfX''; yourself'.
	PkgFix dir: 'ConfMid' manifest: 'PackageSpec new name: ''ConfMid''; requires: ''ConfX'' path: ''../ConfX2''; yourself'.
	PkgFix dir: 'ConfRoot' manifest: 'PackageSpec new name: ''ConfRoot''; requires: ''ConfX'' path: ''../ConfX1''; requires: ''ConfMid'' path: ''../ConfMid''; yourself'.
	t expect: 'two sources for one package name is an error' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'ConfRoot') ].

	"declared name must match the required name"
	PkgFix dir: 'MisDir' manifest: 'PackageSpec new name: ''SomethingElse''; yourself'.
	PkgFix dir: 'MisRoot' manifest: 'PackageSpec new name: ''MisRoot''; requires: ''MisDir'' path: ''../MisDir''; yourself'.
	t expect: 'a name mismatch is an error' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'MisRoot') ].

	"version check is exact in v1"
	PkgFix dir: 'VerDep' manifest: 'PackageSpec new name: ''VerDep''; version: ''1.0''; yourself'.
	PkgFix dir: 'VerRoot' manifest: 'PackageSpec new name: ''VerRoot''; requires: ''VerDep'' path: ''../VerDep'' version: ''2.0''; yourself'.
	t expect: 'a version mismatch is an error' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'VerRoot') ].

	"a manifest must answer a PackageSpec"
	PkgFix dir: 'BadKind' manifest: '42'.
	t expect: 'a manifest answering a non-spec is an error' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'BadKind') ].

	"missing manifest"
	t expect: 'a directory without package.st is an error' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'NoSuchDir') ].

	"unresolved names fail the load, allowUnresolved: overrides"
	PkgFix dir: 'Unres' manifest: 'PackageSpec new name: ''Unres''; files: #(''src/U.st''); yourself'
		file: 'src/U.st' source: 'UnresUser := Object [ boom [ ^UnresMissingXyz new ] ]'.
	t expect: 'a vivified name with no definition fails the load' raises: Error
		in: [ PackageLoader new load: (Path join: PkgFix root with: 'Unres') ].
	PkgFix dir: 'UnresOk' manifest: 'PackageSpec new name: ''UnresOk''; files: #(''src/U.st''); allowUnresolved: #(#UnresLateXyz); yourself'
		file: 'src/U.st' source: 'UnresOkUser := Object [ boom [ ^UnresLateXyz new ] ]'.
	t check: 'allowUnresolved: lets a late-bound name through' that: [
		PackageLoader new load: (Path join: PkgFix root with: 'UnresOk').
		true ].

	PkgFix teardown.
	^t report
]
