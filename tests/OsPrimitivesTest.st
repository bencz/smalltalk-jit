"Functional coverage for the appended OS/system primitives: monotonic clock,
 local UTC offset, entropy, environment, command line, CPU count and the
 file-system calls. The wrappers live on a test-local class on purpose: the
 kernel-side API (System, File, Directory) arrives in a later wave and will
 reuse the same primitives. Expects to run from the repo root (run_tests.sh
 guarantees that) for the stat/listDir checks on 'tests' and 'run_tests.sh'."

OsPrimProbe := Object [

	class monotonicNanos [
		<primitive: MonotonicNanoTimePrimitive>
	]

	class localUtcOffsetAt: epochSeconds [
		<primitive: LocalUtcOffsetPrimitive>
	]

	class randomBytesInto: aByteArray [
		<primitive: RandomBytesPrimitive>
		^nil
	]

	class getEnv: aString [
		<primitive: GetEnvPrimitive>
	]

	class commandLine [
		<primitive: CommandLinePrimitive>
	]

	class cpuCount [
		<primitive: CpuCountPrimitive>
	]

	class statPath: aString [
		<primitive: FileStatPrimitive>
	]

	class mkdir: aString [
		<primitive: FileMkdirPrimitive>
	]

	class unlink: aString [
		<primitive: FileUnlinkPrimitive>
	]

	class rmdir: aString [
		<primitive: FileRmdirPrimitive>
	]

	class rename: fromString to: toString [
		<primitive: FileRenamePrimitive>
	]

	class listDir: aString [
		<primitive: FileListDirPrimitive>
	]

	class cwd [
		<primitive: FileGetCwdPrimitive>
	]

	class chdir: aString [
		<primitive: FileChdirPrimitive>
	]

	class realpath: aString [
		<primitive: FileRealpathPrimitive>
	]
]

[ | t a b dir dir2 base entries before after off stat cl saved |
  t := TestRun named: 'OsPrimitives'.

  "monotonic clock: positive and non-decreasing"
  before := OsPrimProbe monotonicNanos.
  after := OsPrimProbe monotonicNanos.
  t check: 'monotonic positive' that: [ before > 0 ].
  t check: 'monotonic non-decreasing' that: [ after >= before ].

  "local UTC offset: an integer within 16 hours either way"
  off := OsPrimProbe localUtcOffsetAt: 1750000000.
  t check: 'utc offset integer' that: [ off isKindOf: Integer ].
  t check: 'utc offset range' that: [ off abs <= (16 * 3600) ].

  "entropy: two 32-byte draws differ (all-zero or equal draws mean broken)"
  a := ByteArray new: 32.
  b := ByteArray new: 32.
  OsPrimProbe randomBytesInto: a.
  OsPrimProbe randomBytesInto: b.
  t check: 'random draws differ' that: [ a ~= b ].
  t check: 'random fails on non-bytes' that: [ (OsPrimProbe randomBytesInto: 42) isNil ].

  "environment"
  t check: 'getenv PATH' that: [ (OsPrimProbe getEnv: 'PATH') notNil ].
  t check: 'getenv missing is nil' that: [ (OsPrimProbe getEnv: 'ST_SURELY_MISSING_VAR_12345') isNil ].

  "command line: the runner passes no extra arguments after -f"
  cl := OsPrimProbe commandLine.
  t check: 'command line empty' that: [ cl isEmpty ].

  "cpu count"
  t check: 'cpu count at least 1' that: [ OsPrimProbe cpuCount >= 1 ].

  "cwd / realpath"
  t check: 'cwd non-empty' that: [ OsPrimProbe cwd size > 0 ].
  t check: 'realpath of dot is cwd' that: [ (OsPrimProbe realpath: '.') = OsPrimProbe cwd ].
  t check: 'realpath missing nil' that: [ (OsPrimProbe realpath: '/no-such-path-xyz/deep') isNil ].

  "stat: {type. size. mtimeMillis}, nil for missing"
  stat := OsPrimProbe statPath: 'tests'.
  t check: 'stat dir type 2' that: [ stat notNil and: [ (stat at: 1) = 2 ] ].
  t check: 'stat file type 1' that: [ ((OsPrimProbe statPath: 'run_tests.sh') at: 1) = 1 ].
  t check: 'stat file size positive' that: [ ((OsPrimProbe statPath: 'run_tests.sh') at: 2) > 0 ].
  t check: 'stat mtime positive' that: [ ((OsPrimProbe statPath: 'run_tests.sh') at: 3) > 0 ].
  t check: 'stat missing nil' that: [ (OsPrimProbe statPath: 'no-such-file-xyz') isNil ].

  "listDir"
  entries := OsPrimProbe listDir: 'tests'.
  t check: 'listDir finds this test' that: [ entries includes: 'OsPrimitivesTest.st' ].
  t check: 'listDir missing nil' that: [ (OsPrimProbe listDir: 'no-such-dir-xyz') isNil ].

  "mkdir / stat / rename / rmdir roundtrip under /tmp"
  base := '/tmp/st-osprim-', OsPrimProbe monotonicNanos printString.
  dir := base, '-a'.
  dir2 := base, '-b'.
  t check: 'mkdir' that: [ OsPrimProbe mkdir: dir ].
  t check: 'mkdir stat type 2' that: [ ((OsPrimProbe statPath: dir) at: 1) = 2 ].
  t check: 'mkdir existing false' that: [ (OsPrimProbe mkdir: dir) = false ].
  t check: 'rename' that: [ OsPrimProbe rename: dir to: dir2 ].
  t check: 'renamed old gone' that: [ (OsPrimProbe statPath: dir) isNil ].
  t check: 'rmdir' that: [ OsPrimProbe rmdir: dir2 ].
  t check: 'rmdir gone' that: [ (OsPrimProbe statPath: dir2) isNil ].
  t check: 'unlink missing false' that: [ (OsPrimProbe unlink: dir2) = false ].

  "chdir roundtrip"
  saved := OsPrimProbe cwd.
  t check: 'chdir tmp' that: [ OsPrimProbe chdir: '/tmp' ].
  t check: 'chdir took effect' that: [ OsPrimProbe cwd = (OsPrimProbe realpath: '/tmp') ].
  t check: 'chdir back' that: [ OsPrimProbe chdir: saved ].
  t check: 'cwd restored' that: [ OsPrimProbe cwd = saved ].

  ^t report
]
