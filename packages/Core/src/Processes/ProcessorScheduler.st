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


	"Read-and-clear the VM's unhandled-error counter (bumped by
	 Exception>>defaultAction before it kills the signaling fiber). A nonzero
	 count at process end forces a nonzero exit code, so a test that
	 DELIBERATELY drives an error into defaultAction must take the count,
	 assert on it, and thereby keep its run green."
	takeUnhandledErrorCount [
		<primitive: UnhandledErrorTakePrimitive>
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
		"Acquire stripe 0 of the striped sync monitor (the legacy global lock). Prefer
		 monitorEnterOn: with the sync object so unrelated objects do not serialize on one
		 lock. Critical sections MUST stay flat — never send monitorEnter/On: while already
		 holding the monitor."
		<primitive: MonitorEnterPrimitive>
	]


	monitorEnterOn: anObject [
		"Acquire the sync-monitor STRIPE for anObject (the sync object whose state this
		 section protects and whose waiter this section may resume). Many unrelated sync
		 objects run in parallel across the stripes instead of serializing on one global
		 lock. The stripe is stashed, so the matching monitorExit/parkOnMonitor release it
		 without needing the object again. Critical sections MUST stay flat."
		<primitive: MonitorEnterOnPrimitive>
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
