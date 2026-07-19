# Packages and projects

The kernel image (`smalltalk/`) is only the core: compiler, collections,
streams, files, concurrency, exceptions, the package tooling itself. Every
other library lives in a package under `packages/` (the shipped stdlib uses
the `Std.` prefix: `Std.Http`, `Std.Json`, `Std.Actors`, `Std.Uuid`,
`Std.Base64`), and applications are packages with an entry point.

## The manifest: package.st

A package is a directory with a `package.st` manifest: ONE cascade
expression that answers a `PackageSpec`. It is data, not a class definition;
the loader evaluates it and a build leaves no residue in the image.

```smalltalk
"A library."
PackageSpec new
	name: 'Std.Http';
	version: '0.1.0';
	summary: 'HTTP/1.1 server and client';
	requires: 'Std.Json';
	files: #(
		'src/HttpRequest.st'
		'src/HttpServer.st');
	testFiles: #('tests/HttpTest.st');
	yourself
```

```smalltalk
"An application: entry: makes it runnable."
PackageSpec new
	name: 'TodoApi';
	version: '1.0.0';
	requires: 'Std.Http';
	requires: 'Acme.Postgres' path: '../acme-postgres';
	files: #('src/TodoStore.st' 'src/Main.st');
	testFiles: #('tests/TodoApiTest.st');
	entry: #Main;
	yourself
```

- `requires: 'Name'` resolves BY NAME against the package roots (below).
  This is the primary form; a user project never spells a path to the stdlib.
- `requires: 'Name' path: 'relative/or/absolute'` is the local override for
  development and vendoring; it wins over every root. Relative paths anchor
  at the manifest's directory.
- `requires: 'Name' version: '1.0'` additionally checks the resolved
  package's version (exact match today; ranges arrive with the registry).
- `files:` is an explicit ordered list, superclass before subclass, the same
  practice as the kernel bootstrap.
- `entry: #Main` declares the application entry point; the selector defaults
  to `main:` (`entry:selector:` overrides) and receives the command-line
  arguments Array; an integer answer becomes the process exit code.
- `allowUnresolved: #(#Name ...)` exempts names from the end-of-load
  unresolved check (for globals legitimately assigned later at run time).

Package names are IDs; dots are allowed and encouraged for third parties
(`Acme.Postgres`). The package's directory is named after the ID.

## Package roots (requires-by-name search order)

1. the `requires:path:` override, when present;
2. each entry of `ST_PACKAGE_PATH` (colon-separated), in order;
3. the user store `~/.st/packages/`;
4. the dist store next to the `st` executable: `packages/` beside it, then
   `../packages` (covers running from a build tree).

Each root is searched for `<root>/<Name>/package.st`. A name found nowhere
fails the build listing every directory searched. One package name must
resolve to one directory: two different sources for the same name is an
error, as is a dependency cycle (reported with the full chain).

A future registry (`st add`, lockfile, semver ranges) plugs into this
resolver without changing the manifest format.

## The CLI

```
st new <name>   scaffold a project (package.st, src/Main.st, tests/)
st build        compile the project into .stbuild/program.img (--force rebuilds)
st run [args]   build if stale, then invoke the entry point
st test         build if stale, then run the project's testFiles
st repl         REPL inside the project image and namespace
```

Every command finds the project by walking upward from the working directory
to the nearest `package.st`. The base image resolves as: `-s` flag, then the
`ST_IMAGE` env var, then a `snapshot` next to the executable, then
`./snapshot`.

`st build` loads the dependency graph in topological order on top of the
base image, records the entry point IN the image, snapshots it to
`.stbuild/program.img` (before any fiber ever starts, exactly like the
kernel bootstrap), and writes `.stbuild/build.deps`: one
`<mtimeMillis><TAB><path>` line per manifest and source that participated.
A later `st run` compares those mtimes (plus the image header, the st binary
and the base image) and skips straight to execution when nothing moved;
`st run` after an edit rebuilds and continues in the same process. Test
files are compiled fresh on every `st test` and are never snapshotted.

Package `initialize` methods run at build time, before the scheduler exists,
so they must not spawn fibers or wait; do that from the entry point.

## Tests

`testFiles` are TestRun scripts (classes plus top-level blocks whose last
block answers `^t report`). `st test` compiles them into a `<Name>Tests`
namespace importing the package under test and its direct requirements, runs
each file through the same path as `st -f`, and exits with the summed fail
count. `run_tests.sh` drives `st test` for every shipped package and builds
the fat `devimage/` project (all Std packages) that the samples run against.
