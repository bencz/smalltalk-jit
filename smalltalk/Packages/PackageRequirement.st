"One requires: entry of a PackageSpec: a package name, an optional local
 path override (dev/vendoring; wins over every search root) and an optional
 exact version check."
PackageRequirement := Object [

	| name path version |


	class name: aName path: aPath version: aVersion [
		| req |
		req := self basicNew.
		req setName: aName path: aPath version: aVersion.
		^req
	]


	setName: aName path: aPath version: aVersion [
		name := aName.
		path := aPath.
		version := aVersion.
	]


	name [
		^name
	]


	path [
		^path
	]


	version [
		^version
	]

]
