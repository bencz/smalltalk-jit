"The fat development image: no sources and no entry point of its own, just
 requires for every shipped Std.* package. run_tests.sh builds this project
 on top of the fresh core image (with ST_PACKAGE_PATH pointing at packages/)
 and runs the samples against the result, so unqualified references to lib
 classes keep working exactly as they did when everything was in the kernel."
PackageSpec new
	name: 'DevImage';
	version: '0.1.0';
	requires: 'Std.Json';
	requires: 'Std.Http';
	requires: 'Std.Actors';
	requires: 'Std.Uuid';
	requires: 'Std.Base64';
	yourself
