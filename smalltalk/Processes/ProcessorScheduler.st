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


	"synchronization monitor (thread-safe sync primitives)"

	monitorEnter [
		"Acquire the VM's global sync monitor. Under the multi-worker pool the sync
		 primitives run their short critical sections under this monitor so that
		 'check a condition then park' is atomic across OS-thread workers. Critical
		 sections MUST stay flat — never send monitorEnter while already holding it."
		<primitive: MonitorEnterPrimitive>
	]


	monitorExit [
		"Release the global sync monitor."
		<primitive: MonitorExitPrimitive>
	]


	parkOnMonitor [
		"Park the current process, atomically releasing the sync monitor. Wakes when
		 another process (holding the monitor) resumes this process's id. The monitor is
		 NOT re-acquired on wake. Closes the lost-wakeup window: the resumer cannot run
		 until this process has published its parked state."
		<primitive: MonitorParkPrimitive>
	]

]
