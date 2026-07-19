"System: process- and OS-level facilities gathered on the class side of a
 stateless class: environment variables, command-line arguments, process exit,
 CPU count, a monotonic clock, the working directory, entropy and standard
 error. Every entry funnels through a private primitive wrapper, mirroring the
 pattern used by DateTime class currentMicroTime."

System := Object [

	"--- environment ---"

	class env: aString [
		"Answer the value of the environment variable named aString, or nil when
		 it is not set."
		^self primitiveGetEnv: aString
	]


	class env: aString ifAbsent: aBlock [
		"Answer the value of the environment variable named aString, or the value
		 of aBlock when it is not set."
		| value |

		value := self primitiveGetEnv: aString.
		^value isNil ifTrue: [aBlock value] ifFalse: [value]
	]


	"--- process ---"

	class arguments [
		"An OrderedCollection of the command-line arguments (Strings) that follow
		 the script, empty when none were passed."
		^self primitiveCommandLine
	]


	class exit: anInteger [
		"Terminate the process immediately with the given status code."
		^self primitiveExit: anInteger
	]


	class exit [
		"Terminate the process with a success (zero) status."
		^self exit: 0
	]


	"--- hardware and clock ---"

	class cpuCount [
		"The number of logical CPUs available to the process (at least 1)."
		^self primitiveCpuCount
	]


	class monotonicNanos [
		"A monotonically non-decreasing clock reading in nanoseconds. It has no
		 relation to wall-clock time; use it only for measuring elapsed time."
		^self primitiveMonotonicNanos
	]


	"--- working directory ---"

	class currentDirectory [
		"The process working directory as a String."
		^self primitiveGetCwd
	]


	class currentDirectory: aString [
		"Change the process working directory to aString. Answer true on success,
		 false on failure."
		^self primitiveChdir: aString
	]


	"--- entropy ---"

	class randomBytes: anInteger [
		"A fresh ByteArray of anInteger bytes filled with operating-system
		 entropy."
		| bytes |

		bytes := ByteArray new: anInteger.
		self primitiveRandomBytes: bytes.
		^bytes
	]


	"--- standard error ---"

	class stderr [
		"The shared ExternalStream over descriptor 2, created at bootstrap
		 alongside Transcript (descriptor 1)."
		^StdErr
	]


	"--- primitive wrappers (private) ---"

	class primitiveGetEnv: aString [
		<primitive: GetEnvPrimitive>
		^nil
	]


	class primitiveCommandLine [
		<primitive: CommandLinePrimitive>
	]


	class primitiveExit: anInteger [
		<primitive: ExitWithCodePrimitive>
	]


	class primitiveCpuCount [
		<primitive: CpuCountPrimitive>
	]


	class executablePath [
		"Absolute path of the running st binary, or nil when the platform
		 cannot tell. Anchors the distributed package store (PackageLoader
		 packageRoots) and the default image search."
		<primitive: ExecutablePathPrimitive>
		^nil
	]


	class flushSendCaches [
		"Reset every JIT send cache (inline caches and per-worker lookup
		 caches) under a stop-the-world bracket. Escape hatch for reflective
		 method-dictionary surgery; class builds, redefinitions and extensions
		 invalidate automatically."
		<primitive: FlushSendCachesPrimitive>
	]


	class primitiveMonotonicNanos [
		<primitive: MonotonicNanoTimePrimitive>
	]


	class primitiveGetCwd [
		<primitive: FileGetCwdPrimitive>
	]


	class primitiveChdir: aString [
		<primitive: FileChdirPrimitive>
	]


	class primitiveRandomBytes: aByteArray [
		<primitive: RandomBytesPrimitive>
		^nil
	]

]
