"Std.Json package manifest. Self-contained JSON codec (RFC 8259): C-backed
 parser/encoder primitives with a reflective Smalltalk fallback protocol
 (jsonOn:)."
PackageSpec new
	name: 'Std.Json';
	version: '0.1.0';
	summary: 'JSON codec (RFC 8259) over the VM json primitives';
	files: #('src/Json.st');
	testFiles: #('tests/JsonTest.st' 'tests/JsonExtraTest.st');
	yourself
