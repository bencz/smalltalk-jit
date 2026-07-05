ProcessorScheduler := Object [

	class initialize [
		Processor := self new.
	]


	"current process"

	currentId [
		<primitive: ProcessCurrentIdPrimitive>
	]


	thisProcess [
		^Process fromId: self currentId
	]


	"scheduling"

	yield [
		"Give other ready processes a chance to run."
		<primitive: ProcessYieldPrimitive>
	]


	resumeId: anId [
		<primitive: ProcessResumePrimitive>
	]


	terminateId: anId [
		<primitive: ProcessTerminatePrimitive>
	]


	"blocking"

	suspendCurrent [
		"Park the current process off the run queue until it is resumed."
		<primitive: ProcessSuspendPrimitive>
	]


	sleepMicroseconds: micros [
		"Park the current process for at least `micros` microseconds."
		<primitive: ProcessSleepPrimitive>
	]

]
