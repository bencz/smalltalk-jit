"PackageSpec manifest DSL: a package.st is one cascade expression answering
 a PackageSpec. Covers evaluation through the compiler, defaults, the
 requires: forms and validation."

[ | t spec |
	t := TestRun named: 'PackageSpec'.

	spec := Compiler new evaluate: 'PackageSpec new name: ''Probe''; version: ''1.0''; yourself'.
	t check: 'a manifest expression answers a PackageSpec'
		that: [ spec isKindOf: PackageSpec ].
	t check: 'name is recorded' equals: 'Probe' is: [ spec name ].
	t check: 'version is recorded' equals: '1.0' is: [ spec version ].
	t check: 'the default entry selector is main:' equals: #main: is: [ spec entrySelector ].
	t check: 'no entry: means library' that: [ spec isApplication not ].
	t check: 'files default to empty' that: [ spec files isEmpty ].
	t check: 'testFiles default to empty' that: [ spec testFiles isEmpty ].

	spec := PackageSpec new.
	spec name: 'App'.
	spec entry: #Boot selector: #start:.
	t check: 'entry:selector: records the class' equals: #Boot is: [ spec entryClassName ].
	t check: 'entry:selector: overrides the selector' equals: #start: is: [ spec entrySelector ].
	t check: 'an entry point makes it an application' that: [ spec isApplication ].

	spec := PackageSpec new.
	spec requires: 'Acme.Json'.
	spec requires: 'Acme.Pg' path: '../pg'.
	spec requires: 'Std.Http' version: '1.0'.
	t check: 'requirements accumulate in order' equals: 3 is: [ spec requirements size ].
	t check: 'a name-only requirement has no path'
		that: [ (spec requirements at: 1) path isNil ].
	t check: 'a path requirement keeps its path'
		equals: '../pg' is: [ (spec requirements at: 2) path ].
	t check: 'a version requirement keeps its version'
		equals: '1.0' is: [ (spec requirements at: 3) version ].

	t expect: 'a manifest without a name fails validation' raises: Error
		in: [ PackageSpec new validateFrom: 'somewhere/package.st' ].

	^t report
]
