"File / Directory / Path end-to-end under a unique /tmp directory. Everything
 created here is removed in an ensure: block so a failure never leaves litter
 behind. The file content deliberately mixes a newline and low control bytes to
 prove the read/write path is byte-exact, not text-mangling."

[ | t base dir sub file file2 low content back names |
  t := TestRun named: 'FileApi'.

  base := '/tmp/st-onda5-fileapi-', System monotonicNanos printString.
  dir := base, '-dir'.
  sub := Path join: dir with: 'nested'.
  file := Path join: dir with: 'data.txt'.
  file2 := Path join: dir with: 'renamed.log'.

  low := String new: 3.
  low at: 1 put: (Character codePoint: 1).
  low at: 2 put: (Character codePoint: 2).
  low at: 3 put: (Character codePoint: 7).
  content := 'line one', (String with: Character lf), 'line two', low.

  [
    "--- Path (pure string manipulation) ---"
    t check: 'Path join simple' equals: '/a/b/c' is: [ Path join: '/a/b' with: 'c' ].
    t check: 'Path join no double slash' equals: '/a/b/c' is: [ Path join: '/a/b/' with: 'c' ].
    t check: 'Path join both slashes collapse' equals: '/a/b/c' is: [ Path join: '/a/b/' with: '/c' ].
    t check: 'Path basename' equals: 'data.txt' is: [ Path basename: file ].
    t check: 'Path dirname' equals: dir is: [ Path dirname: file ].
    t check: 'Path extension present' equals: 'txt' is: [ Path extension: 'data.txt' ].
    t check: 'Path extension absent' equals: '' is: [ Path extension: 'README' ].
    t check: 'Path isAbsolute true' that: [ Path isAbsolute: '/x/y' ].
    t check: 'Path isAbsolute false' that: [ (Path isAbsolute: 'x/y') not ].

    "--- Directory create / exists ---"
    t check: 'Directory create' that: [ Directory create: dir ].
    t check: 'Directory exists after create' that: [ Directory exists: dir ].
    t check: 'File exists false before write' that: [ (File exists: file) not ].

    "--- File write / read roundtrip (byte-exact) ---"
    File writeAll: content to: file.
    t check: 'File exists after write' that: [ File exists: file ].
    t check: 'File size matches content' equals: content size is: [ File size: file ].
    back := File readAll: file.
    t check: 'readAll roundtrip byte-exact' that: [ back = content ].

    "--- metadata ---"
    t check: 'modificationTimeMillis positive' that: [ (File modificationTimeMillis: file) > 0 ].
    t check: 'modificationTime is a DateTime' that: [ (File modificationTime: file) isKindOf: DateTime ].
    t check: 'modificationTime year plausible' that: [ (File modificationTime: file) year >= 2026 ].

    "--- listing ---"
    Directory create: sub.
    names := Directory entries: dir.
    t check: 'entries includes the file' that: [ names includes: 'data.txt' ].
    t check: 'entries includes the subdir' that: [ names includes: 'nested' ].
    t check: 'filesIn: contains the file path' that: [ (Directory filesIn: dir) includes: file ].
    t check: 'filesIn: excludes the subdir' that: [ ((Directory filesIn: dir) includes: sub) not ].
    t check: 'directoriesIn: contains the subdir path' that: [ (Directory directoriesIn: dir) includes: sub ].
    t check: 'directoriesIn: excludes the file' that: [ ((Directory directoriesIn: dir) includes: file) not ].

    "--- rename / delete ---"
    t check: 'File rename' that: [ File rename: file to: file2 ].
    t check: 'renamed old gone' that: [ (File exists: file) not ].
    t check: 'renamed new present' that: [ File exists: file2 ].
    t check: 'File delete renamed' that: [ File delete: file2 ].
    t check: 'deleted gone' that: [ (File exists: file2) not ].
    t check: 'delete missing returns false' that: [ (File delete: file2) = false ].
    t check: 'Directory delete non-empty fails' that: [ (Directory delete: dir) = false ].
    Directory delete: sub.
    t check: 'Directory delete empty succeeds' that: [ Directory delete: dir ].
    t check: 'Directory gone' that: [ (Directory exists: dir) not ].
  ] ensure: [
    "Belt and braces: remove anything still present, ignoring failures."
    File delete: file.
    File delete: file2.
    Directory delete: sub.
    Directory delete: dir.
  ].

  ^t report
]
