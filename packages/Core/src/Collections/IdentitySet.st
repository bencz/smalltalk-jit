"An IdentitySet is a Set that compares its elements by identity (==) and
 hashes them with identityHash, instead of the value-based = / hash used by
 Set. Two value-equal but non-identical objects therefore occupy distinct
 slots; identical objects (for example the same Symbol) collapse to one."

IdentitySet := Set [

	findIndex: anObject [
		| size index object |

		size := contents size.
		index := (anObject identityHash bitAnd: size - 1) + 1.

		[object := contents at: index.
		(object isNil or: [object == anObject]) ifTrue: [^index].
		index := index == size ifTrue: [1] ifFalse: [index + 1]]
			repeat.
	]

]
