"The application: requires each local module by path. Every module loads
 into its own namespace and Main's compiles see all of them through the
 imports (declaration order matters: on a name two imports share, the FIRST
 one wins, so Banner's Formatter shadows Plain's here)."
PackageSpec new
	name: 'NsShowcase';
	version: '0.1.0';
	requires: 'Greetings' path: '../Greetings';
	requires: 'Numbers' path: '../Numbers';
	requires: 'Text' path: '../Text';
	requires: 'Banner' path: '../Banner';
	requires: 'Plain' path: '../Plain';
	files: #('src/Main.st');
	testFiles: #('tests/AppTest.st');
	entry: #Main;
	yourself
