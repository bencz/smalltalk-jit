"System: the class-side OS facade over the environment, arguments, CPU count,
 monotonic clock, working directory and entropy primitives. exit: is NOT
 exercised here (it would end the test process)."

[ | t a b before after cwd |
  t := TestRun named: 'System'.

  "environment"
  t check: 'env: PATH is set' that: [ (System env: 'PATH') notNil ].
  t check: 'env: missing is nil' that: [ (System env: 'ST_SURELY_MISSING_VAR_54321') isNil ].
  t check: 'env:ifAbsent: returns value when present'
      that: [ (System env: 'PATH' ifAbsent: [ #absent ]) ~= #absent ].
  t check: 'env:ifAbsent: runs block when absent'
      equals: #absent
      is: [ System env: 'ST_SURELY_MISSING_VAR_54321' ifAbsent: [ #absent ] ].

  "arguments: the runner passes none after the script"
  t check: 'arguments is a collection' that: [ System arguments isKindOf: OrderedCollection ].
  t check: 'arguments isEmpty' that: [ System arguments isEmpty ].

  "cpu count"
  t check: 'cpuCount at least 1' that: [ System cpuCount >= 1 ].

  "monotonic clock: non-decreasing and integer"
  before := System monotonicNanos.
  after := System monotonicNanos.
  t check: 'monotonicNanos is Integer' that: [ before isKindOf: Integer ].
  t check: 'monotonicNanos non-decreasing' that: [ after >= before ].
  t check: 'monotonicNanos positive' that: [ before > 0 ].

  "working directory"
  cwd := System currentDirectory.
  t check: 'currentDirectory non-empty' that: [ cwd notNil and: [ cwd size > 0 ] ].
  t check: 'currentDirectory: back to self succeeds' that: [ System currentDirectory: cwd ].
  t check: 'currentDirectory unchanged' that: [ System currentDirectory = cwd ].

  "entropy: right size and two draws differ"
  a := System randomBytes: 16.
  b := System randomBytes: 16.
  t check: 'randomBytes: is a ByteArray' that: [ a isKindOf: ByteArray ].
  t check: 'randomBytes: 16 has size 16' that: [ a size = 16 ].
  t check: 'two random draws differ' that: [ a ~= b ].

  "stderr global exists and is a stream over fd 2"
  t check: 'stderr is an ExternalStream' that: [ System stderr isKindOf: ExternalStream ].

  ^t report
]
