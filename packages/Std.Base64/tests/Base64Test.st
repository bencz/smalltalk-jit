"Base64: RFC 4648 test vectors (the classic 'foobar' progression), a full
 0..255 byte roundtrip, and rejection of malformed input. encode: accepts a
 String or ByteArray; decode: answers a String of the decoded bytes."

[ | t bytes expected encoded decoded ok |
  t := TestRun named: 'Base64'.

  "--- RFC 4648 section 10 vectors ---"
  t check: 'encode empty' equals: '' is: [ Base64 encode: '' ].
  t check: 'encode f' equals: 'Zg==' is: [ Base64 encode: 'f' ].
  t check: 'encode fo' equals: 'Zm8=' is: [ Base64 encode: 'fo' ].
  t check: 'encode foo' equals: 'Zm9v' is: [ Base64 encode: 'foo' ].
  t check: 'encode foob' equals: 'Zm9vYg==' is: [ Base64 encode: 'foob' ].
  t check: 'encode fooba' equals: 'Zm9vYmE=' is: [ Base64 encode: 'fooba' ].
  t check: 'encode foobar' equals: 'Zm9vYmFy' is: [ Base64 encode: 'foobar' ].

  t check: 'decode empty' equals: '' is: [ Base64 decode: '' ].
  t check: 'decode f' equals: 'f' is: [ Base64 decode: 'Zg==' ].
  t check: 'decode fo' equals: 'fo' is: [ Base64 decode: 'Zm8=' ].
  t check: 'decode foo' equals: 'foo' is: [ Base64 decode: 'Zm9v' ].
  t check: 'decode foob' equals: 'foob' is: [ Base64 decode: 'Zm9vYg==' ].
  t check: 'decode fooba' equals: 'fooba' is: [ Base64 decode: 'Zm9vYmE=' ].
  t check: 'decode foobar' equals: 'foobar' is: [ Base64 decode: 'Zm9vYmFy' ].

  "--- encode also accepts a ByteArray ---"
  t check: 'encode ByteArray of foobar'
      equals: 'Zm9vYmFy'
      is: [ | src ba |
            src := #(102 111 111 98 97 114).
            ba := ByteArray new: src size.
            1 to: src size do: [ :i | ba at: i put: (src at: i) ].
            Base64 encode: ba ].

  "--- full 0..255 roundtrip is byte-exact ---"
  bytes := ByteArray new: 256.
  1 to: 256 do: [ :i | bytes at: i put: i - 1 ].
  expected := String new: 256.
  1 to: 256 do: [ :i | expected at: i put: (Character codePoint: i - 1) ].
  encoded := Base64 encode: bytes.
  decoded := Base64 decode: encoded.
  t check: 'roundtrip 0..255 length' equals: 256 is: [ decoded size ].
  ok := true.
  1 to: 256 do: [ :i | (decoded at: i) = (expected at: i) ifFalse: [ ok := false ] ].
  t check: 'roundtrip 0..255 byte-exact' that: [ ok ].
  t check: 'roundtrip equals expected string' that: [ decoded = expected ].

  "--- malformed input is rejected with a clear error ---"
  t expect: 'decode rejects bad length' raises: InvalidArgumentError in: [ Base64 decode: 'Zm9' ].
  t expect: 'decode rejects illegal char' raises: InvalidArgumentError in: [ Base64 decode: 'Zm9*' ].
  t expect: 'encode rejects a non-collection' raises: InvalidArgumentError in: [ Base64 encode: 42 ].

  ^t report
]
