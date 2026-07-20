Process := Object [

	| id |


	"instance creation"

	class fromId: anId [
		^self new setId: anId
	]


	"accessing"

	id [
		^id
	]


	setId: anId [
		id := anId.
		^self
	]


	"scheduling"

	resume [
		"Move this process into the ready queue."
		Processor resumeId: id.
		^self
	]


	terminate [
		"Stop this process. If it is the current process this never returns."
		Processor terminateId: id.
		^self
	]

]
