"Direct functional coverage of FileStream / ExternalStream / BufferedStream on
 real descriptors, which had no functional test before this wave. Covers file
 creation on write: and readOrWrite:, chunked reads (next / next: / atEnd),
 1-based repositioning via position:, and the IoError raised when opening a
 missing file for reading. Everything is cleaned up in an ensure: block.

 Note on the API actually present: FileStream positioning is 1-based
 (position: 1 is the first byte); #reset is inherited as SubClassResponsibility
 and is NOT usable on a FileStream, so this test reposition with position:."

[ | t missing wpath rwpath s c1 c2 c3 tail |
  t := TestRun named: 'FileStream'.

  missing := '/tmp/st-onda5-fs-missing-', System monotonicNanos printString.
  wpath := '/tmp/st-onda5-fs-w-', System monotonicNanos printString, '.txt'.
  rwpath := '/tmp/st-onda5-fs-rw-', System monotonicNanos printString, '.txt'.

  [
    "--- opening a missing file for reading raises IoError (the open primitive
     fails and FileStream signals the last IoError) ---"
    t check: 'read: missing file not present first' that: [ (File exists: missing) not ].
    t expect: 'read: missing raises IoError' raises: IoError in: [ FileStream read: missing ].
    t expect: 'read: missing raises (also an Error)' raises: Error in: [ FileStream read: missing ].
    t check: 'read: missing did not create it' that: [ (File exists: missing) not ].

    "--- write: creates a brand-new file, nextPutAll: + flush + close ---"
    t check: 'write target absent first' that: [ (File exists: wpath) not ].
    s := FileStream write: wpath.
    s nextPutAll: 'ABCDE'.
    s nextPut: $F.
    s nextPutAll: 'GHIJ'.
    s flush.
    s close.
    t check: 'write created the file' that: [ File exists: wpath ].
    t check: 'written bytes are exact' equals: 'ABCDEFGHIJ' is: [ File readAll: wpath ].

    "--- read: chunked reads with next / next: / atEnd ---"
    s := FileStream read: wpath.
    c1 := s next.
    c2 := s next: 3.
    c3 := s next: 6.
    t check: 'next reads one Character' that: [ c1 = $A ].
    t check: 'next: reads a run' equals: 'BCD' is: [ c2 ].
    t check: 'next: reads the remainder' equals: 'EFGHIJ' is: [ c3 ].
    t check: 'atEnd true after consuming all' that: [ s atEnd ].

    "--- position: repositions (1-based) so we can re-read ---"
    s position: 1.
    t check: 'position: 1 rewinds to first byte' that: [ s next = $A ].
    s position: 5.
    tail := s next: 2.
    t check: 'position: 5 lands on 5th byte' equals: 'EF' is: [ tail ].
    s close.

    "--- readOrWrite: creates a file, writes, repositions and reads back ---"
    t check: 'readOrWrite target absent first' that: [ (File exists: rwpath) not ].
    s := FileStream readOrWrite: rwpath.
    s nextPutAll: 'roundtrip'.
    s flush.
    s position: 1.
    t check: 'readOrWrite created and reads back' equals: 'roundtrip' is: [ s next: 9 ].
    s close.
    t check: 'readOrWrite file persisted' equals: 'roundtrip' is: [ File readAll: rwpath ].
  ] ensure: [
    File delete: wpath.
    File delete: rwpath.
    File delete: missing.
  ].

  ^t report
]
