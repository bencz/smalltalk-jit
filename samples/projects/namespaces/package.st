"ONE project, several namespaces: each src file declares its own with the
 `Name := Namespace [ classDefs ]` form. No extra packages involved; use
 packages (see ../modules) when a module needs its own dependencies or wants
 to be shared between projects."
PackageSpec new
	name: 'NsShowcase';
	version: '0.1.0';
	files: #(
		'src/Greetings.st'
		'src/Numbers.st'
		'src/Text.st'
		'src/Banner.st'
		'src/Plain.st'
		'src/Main.st');
	testFiles: #('tests/AppTest.st');
	entry: #Main;
	yourself
