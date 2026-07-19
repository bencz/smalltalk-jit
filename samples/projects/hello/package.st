"The smallest possible application project (what st new scaffolds)."
PackageSpec new
	name: 'Hello';
	version: '0.1.0';
	files: #('src/Main.st');
	testFiles: #('tests/MainTest.st');
	entry: #Main;
	yourself
