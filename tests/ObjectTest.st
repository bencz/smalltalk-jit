TestIndexedObject := Object [

	<shape: IndexedShape>

]


[
	| object stream |

	ObjRun := TestRun named: 'Object'.

	object := Object new.

	ObjRun expect: 'at: -1 raises' raises: OutOfRangeError in: [object at: -1].
	ObjRun expect: 'at: 0 raises' raises: OutOfRangeError in: [object at: 0].
	ObjRun expect: 'at: 1 raises' raises: OutOfRangeError in: [object at: 1].

	ObjRun expect: 'at: -1 put: raises' raises: OutOfRangeError in: [object at: -1 put: nil].
	ObjRun expect: 'at: 0 put: raises' raises: OutOfRangeError in: [object at: 0 put: nil].
	ObjRun expect: 'at: 1 put: raises' raises: OutOfRangeError in: [object at: 1 put: nil].

	ObjRun expect: 'basicAt: -1 raises' raises: OutOfRangeError in: [object basicAt: -1].
	ObjRun expect: 'basicAt: 0 raises' raises: OutOfRangeError in: [object basicAt: 0].
	ObjRun expect: 'basicAt: 1 raises' raises: OutOfRangeError in: [object basicAt: 1].

	ObjRun expect: 'basicAt: -1 put: raises' raises: OutOfRangeError in: [object basicAt: -1 put: nil].
	ObjRun expect: 'basicAt: 0 put: raises' raises: OutOfRangeError in: [object basicAt: 0 put: nil].
	ObjRun expect: 'basicAt: 1 put: raises' raises: OutOfRangeError in: [object basicAt: 1 put: nil].

	ObjRun check: 'size is 0' that: [object size == 0].
	ObjRun check: 'basicSize is 0' that: [object basicSize == 0].
	ObjRun check: 'yourself is receiver' that: [object yourself == object].

	ObjRun expect: 'instVarAt: -1 raises' raises: OutOfRangeError in: [object instVarAt: -1].
	ObjRun expect: 'instVarAt: 0 raises' raises: OutOfRangeError in: [object instVarAt: 0].
	ObjRun expect: 'instVarAt: 1 raises' raises: OutOfRangeError in: [object instVarAt: 1].

	ObjRun expect: 'instVarAt: -1 put: raises' raises: OutOfRangeError in: [object instVarAt: -1 put: nil].
	ObjRun expect: 'instVarAt: 0 put: raises' raises: OutOfRangeError in: [object instVarAt: 0 put: nil].
	ObjRun expect: 'instVarAt: 1 put: raises' raises: OutOfRangeError in: [object instVarAt: 1 put: nil].

	ObjRun check: 'equals itself' that: [object = object].
	ObjRun check: 'not equals nil' that: [(object = nil) = false].

	ObjRun check: 'identical to itself' that: [object == object].
	ObjRun check: 'not identical to nil' that: [(object == nil) = false].

	ObjRun check: 'hash positive' that: [object hash > 0].

	ObjRun check: 'not unequal to itself' that: [(object ~= object) = false].
	ObjRun check: 'unequal to nil' that: [object ~= nil].

	ObjRun check: 'not nonidentical to itself' that: [(object ~~ object) = false].
	ObjRun check: 'unequal to nil again' that: [object ~= nil].

	ObjRun check: 'identityHash positive' that: [object identityHash > 0].

	ObjRun check: 'class is Object' that: [object class == Object].
	ObjRun check: 'isKindOf Object' that: [object isKindOf: Object].
	ObjRun check: 'not isKindOf SmallInteger' that: [(object isKindOf: SmallInteger) = false].
	ObjRun check: '1 isKindOf Object' that: [1 isKindOf: Object].
	ObjRun check: 'isMemberOf Object' that: [object isMemberOf: Object].
	ObjRun check: 'not isMemberOf SmallInteger' that: [(object isMemberOf: SmallInteger) = false].
	ObjRun check: 'respondsTo size' that: [object respondsTo: #size].
	ObjRun check: 'not respondsTo foo' that: [(object respondsTo: #foo) = false].

	"Assert do: [object doesNotUnderstand: ]"

	ObjRun check: 'printString' equals: 'an Object' is: [object printString].
	stream := CollectionStream with: (String new: 32).
	object examineOn: stream.
	ObjRun check: 'examineOn contents' equals: ('an Object' copyWith: Character lf) is: [stream contents].
]


[
	| object |

	object := TestIndexedObject new: 8.

	ObjRun expect: 'indexed at: -1 raises' raises: OutOfRangeError in: [object at: -1].
	ObjRun check: 'indexed at: 1 starts nil' that: [(object at: 1) == nil].
	ObjRun check: 'indexed at: 8 starts nil' that: [(object at: 8) == nil].
	ObjRun expect: 'indexed at: 9 raises' raises: OutOfRangeError in: [object at: 9].

	ObjRun expect: 'indexed at: -1 put: raises' raises: OutOfRangeError in: [object at: -1 put: nil].
	ObjRun expect: 'indexed at: 9 put: raises' raises: OutOfRangeError in: [object at: 9 put: nil].

	object at: 1 put: 1.
	ObjRun check: 'indexed at: 1 stores' that: [(object at: 1) == 1].
	object at: 8 put: 2.
	ObjRun check: 'indexed at: 8 stores' that: [(object at: 8) == 2].

	ObjRun check: 'indexed size is 8' that: [object size == 8].
	ObjRun check: 'indexed basicSize is 8' that: [object basicSize == 8].

	ObjRun check: 'indexed printString' equals: 'a TestIndexedObject' is: [object printString].
]


[
	| object object2 |

	object := Object new.
	object2 := Object new.
	object become: object2.
	ObjRun check: 'become swaps identityHash' that: [object identityHash == object2 identityHash].
]


[
	| object object2 |

	object := TestIndexedObject new: 0.
	object2 := TestIndexedObject new: 1.

	object become: object2.
	ObjRun check: 'become swaps indexed identityHash' that: [object identityHash == object2 identityHash].
]


[
	| object |

	object := Object new.

	ObjRun check: 'shallowCopy not identical' that: [(object == object shallowCopy) = false].
	ObjRun check: 'plain object shallowCopy not equal' that: [(object = object shallowCopy) = false].

	object := Array new: 16.
	object at: 1 put: Object new.
	ObjRun check: 'array shallowCopy not identical' that: [(object == object shallowCopy) = false].
	ObjRun check: 'array shallowCopy equal' that: [object = object shallowCopy].
	ObjRun check: 'shallowCopy shares elements' that: [(object at: 1) == (object shallowCopy at: 1)].
	ObjRun check: 'deepCopy copies elements' that: [((object at: 1) == (object deepCopy at: 1)) = false].
]


[ ^ObjRun report ]
