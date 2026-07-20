"An IdentityDictionary is a Dictionary that matches keys by identity (==) and
 hashes them with identityHash. Value-equal but non-identical keys are kept as
 separate entries; identical keys (for example the same Symbol) share one."

IdentityDictionary := Dictionary [

	findIndex: anObject [
		| size index assoc |

		size := contents size.
		index := (anObject identityHash bitAnd: size - 1) + 1.

		[assoc := contents at: index.
		(assoc isNil or: [assoc key == anObject]) ifTrue: [^index].
		index := index == size ifTrue: [1] ifFalse: [index + 1]]
			repeat.
	]

]
