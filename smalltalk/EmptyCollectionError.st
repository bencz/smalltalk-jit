"Raised when an operation needs at least one element but the collection is
 empty (first, last, removeFirst, and similar). A plain, non-resumable Error."

EmptyCollectionError := Error [

	defaultMessageText [
		^'collection is empty'
	]

]
