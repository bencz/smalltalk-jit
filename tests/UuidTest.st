"Uuid generate: canonical 36-char version-4 UUIDs. Verifies the fixed layout
 (length, hyphen positions, version and variant nibbles), that only lowercase
 hex digits appear, and that a batch of generated ids is all-distinct."

UuidProbe := Object [

  class isLowerHexOrHyphen: aString [
    "Every character is either a hyphen at a group boundary or a lowercase hex
     digit; hyphens only at positions 9, 14, 19, 24."
    | hex |
    hex := '0123456789abcdef'.
    1 to: aString size do: [ :i | | ch |
      ch := aString at: i.
      (i = 9 or: [i = 14 or: [i = 19 or: [i = 24]]])
        ifTrue: [ ch = $- ifFalse: [^false] ]
        ifFalse: [ (hex includes: ch) ifFalse: [^false] ] ].
    ^true
  ]
]

[ | t u seen ok |
  t := TestRun named: 'Uuid'.

  u := Uuid generate.

  t check: 'length is 36' equals: 36 is: [ u size ].
  t check: 'hyphen at 9' that: [ (u at: 9) = $- ].
  t check: 'hyphen at 14' that: [ (u at: 14) = $- ].
  t check: 'hyphen at 19' that: [ (u at: 19) = $- ].
  t check: 'hyphen at 24' that: [ (u at: 24) = $- ].
  t check: 'version 4 at position 15' that: [ (u at: 15) = $4 ].
  t check: 'variant in 89ab at position 20' that: [ '89ab' includes: (u at: 20) ].
  t check: 'only lowercase hex and hyphens' that: [ UuidProbe isLowerHexOrHyphen: u ].

  "100 generated ids are all valid and all distinct."
  seen := Set new.
  ok := true.
  100 timesRepeat: [ | g |
    g := Uuid generate.
    (g size = 36
      and: [ (g at: 15) = $4
      and: [ ('89ab' includes: (g at: 20))
      and: [ UuidProbe isLowerHexOrHyphen: g ] ] ]) ifFalse: [ ok := false ].
    seen add: g ].
  t check: '100 ids all well-formed' that: [ ok ].
  t check: '100 ids all distinct' equals: 100 is: [ seen size ].

  ^t report
]
