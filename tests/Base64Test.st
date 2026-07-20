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
  t expect: 'decode rejects = in the middle' raises: InvalidArgumentError in: [ Base64 decode: 'Zm=9' ].
  t expect: 'decode rejects a char above 127' raises: InvalidArgumentError
      in: [ | high |
            high := String new: 4.
            1 to: 4 do: [ :i | high at: i put: (Character codePoint: 200) ].
            Base64 decode: high ].
  t expect: 'encode rejects a non-collection' raises: InvalidArgumentError in: [ Base64 encode: 42 ].

  "--- decodeBytes: answers a ByteArray with the same bytes ---"
  t check: 'decodeBytes answers a ByteArray' that: [ (Base64 decodeBytes: 'Zm9vYmFy') isKindOf: ByteArray ].
  t check: 'decodeBytes foobar bytes'
      that: [ | ba |
              ba := Base64 decodeBytes: 'Zm9vYmFy'.
              ba size = 6 and: [ (ba at: 1) = 102 and: [ (ba at: 6) = 114 ] ] ].
  t check: 'decodeBytes roundtrips 0..255'
      that: [ | ba ok2 |
              ba := Base64 decodeBytes: encoded.
              ok2 := ba size = 256.
              1 to: ba size do: [ :i | (ba at: i) = (i - 1) ifFalse: [ ok2 := false ] ].
              ok2 ].
  t expect: 'decodeBytes rejects bad input' raises: InvalidArgumentError in: [ Base64 decodeBytes: 'Zm9' ].

  "--- encode of a String equals encode of its bytes ---"
  t check: 'String and ByteArray encodes agree'
      that: [ | s ba |
              s := 'any carnal pleasure'.
              ba := ByteArray new: s size.
              1 to: s size do: [ :i | ba at: i put: (s at: i) codePoint ].
              (Base64 encode: s) = (Base64 encode: ba) ].

  "--- big roundtrip: ~100KB forces GC during output allocation, catching a
   stale input pointer not re-read after the allocation ---"
  t check: 'big roundtrip 100KB'
      that: [ | big n dec ok3 |
              n := 100000.
              big := String new: n.
              1 to: n do: [ :i | big at: i put: (Character codePoint: (i * 31 + 7) \\ 256) ].
              dec := Base64 decode: (Base64 encode: big).
              ok3 := dec size = n.
              ok3 ifTrue: [ 1 to: n by: 997 do: [ :i | (dec at: i) = (big at: i) ifFalse: [ ok3 := false ] ] ].
              ok3 and: [ dec = big ] ].

  "--- URL-safe variant: -_ alphabet, no padding ---"
  t check: 'urlsafe encode strips padding' equals: 'Zg' is: [ Base64 encodeUrlSafe: 'f' ].
  t check: 'urlsafe roundtrip f' equals: 'f' is: [ Base64 decodeUrlSafe: 'Zg' ].
  t check: 'urlsafe uses - and _'
      that: [ | ba enc |
              "0xFB 0xEF encodes to '--8=' standard '++8=' -> urlsafe '--8'"
              ba := ByteArray new: 2.
              ba at: 1 put: 251. ba at: 2 put: 239.
              enc := Base64 encodeUrlSafe: ba.
              (enc includes: $+) not and: [ (enc includes: $/) not ] ].
  t check: 'urlsafe roundtrips 0..255'
      that: [ | dec ok4 |
              dec := Base64 decodeBytesUrlSafe: (Base64 encodeUrlSafe: bytes).
              ok4 := dec size = 256.
              1 to: dec size do: [ :i | (dec at: i) = (i - 1) ifFalse: [ ok4 := false ] ].
              ok4 ].
  t expect: 'urlsafe decode rejects impossible length' raises: InvalidArgumentError
      in: [ Base64 decodeUrlSafe: 'Zm9vY' ].

  ^t report
]
