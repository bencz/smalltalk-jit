"Std.Uuid package manifest. RFC 4122 v4 UUIDs over the core entropy
 primitive (System randomBytes:)."
PackageSpec new
	name: 'Std.Uuid';
	version: '0.1.0';
	summary: 'RFC 4122 version 4 UUIDs';
	files: #('src/Uuid.st');
	testFiles: #('tests/UuidTest.st');
	yourself
