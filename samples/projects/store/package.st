"A realistic layered application in ONE project: five namespaces talking to
 each other. Domain (Money value object, catalog, orders), Storage
 (repository, inventory, a custom exception), Events (a block-based bus),
 Billing (tax polymorphism building invoices from Domain instances), Reports
 (aggregation and text tables), and Main wiring a full business day
 together, closing with a fiber-parallel revenue cross-check."
PackageSpec new
	name: 'Store';
	version: '0.1.0';
	files: #(
		'src/Domain.st'
		'src/Storage.st'
		'src/Events.st'
		'src/Billing.st'
		'src/Reports.st'
		'src/Main.st');
	testFiles: #('tests/StoreTest.st');
	entry: #Main;
	yourself
