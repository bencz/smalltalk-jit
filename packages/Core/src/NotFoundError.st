"A lookup (at:, removeKey:, detect:, remove:, indexOf:, keyAtValue:) failed to
 find its target. Modeled as an Error, not a bare Exception: a missing element
 is a genuine failure, not a resumable notification, so on: Error do: catches it
 and resume: is rejected."

NotFoundError := Error [

	| value |


	"instance creation"

	class value: anObject [
		^self new value: anObject
	]


	"accessing"

	value: anObject [
		value := anObject.
	]


	value [
		^value
	]


	defaultMessageText [
		^value printString, ' not found'
	]

]
