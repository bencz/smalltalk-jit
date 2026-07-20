"The samples project: no sources and no entry point of its own, just the
 requires that the sample scripts use. st run <sample>.st finds this manifest
 by walking up from the script's directory and runs the script against this
 project's binary image (.stbuild/program.img, built on demand), so
 unqualified references to Http/Actor/Uuid classes resolve through the
 project namespace imports; Json and Base64 are core and resolve everywhere."
PackageSpec new
	name: 'Samples';
	version: '0.1.0';
	requires: 'Std.Http';
	requires: 'Std.Actors';
	requires: 'Std.Uuid';
	yourself
