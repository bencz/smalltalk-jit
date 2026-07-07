"Isolate is the Smalltalk face of the VM's share-nothing isolates: independent
 VMs, each on its own OS thread/core, with its own heap/GC/scheduler/JIT/kernel
 image. Isolates share NO objects — they communicate only by COPYING values
 through per-isolate inboxes. This is the raw multicore substrate; higher-level
 models (actors, background services, timers) are built on top of it IN SMALLTALK.

 ONE running application spreads work across cores by SPAWNING workers at runtime
 (like Thread/Worker/Ractor) — this is NOT a multi-instance launcher. The four
 primitives are deliberately minimal and know nothing about actors:
   Isolate spawn: sourceString -> start a worker isolate running that program on
                                  another core; the caller keeps running; answers
                                  the worker's id (or -1). Code can't cross the
                                  boundary, so a worker gets SOURCE (it may define
                                  classes + run blocks) + the shared image.
   Isolate currentId           -> which isolate am I (0-based; main is 0)
   Isolate send: obj to: id    -> copy obj by value into isolate `id`'s inbox
   Isolate receive             -> block this process until a value arrives, copied

 `send:to:` answers true on success, false if the target isn't ready yet (retry)
 or the object isn't portable (a graph with blocks/contexts/methods can't cross).
 `receive` parks only the calling green-thread (fiber), so the rest of the isolate
 keeps running; a single receiver process per isolate is the intended pattern."
Isolate := Object [

	class currentId [
		<primitive: IsolateCurrentIdPrimitive>
		^0
	]


	class spawn: aSourceString [
		"Spawn a WORKER isolate (a fresh VM on another OS thread/core) that runs
		 aSourceString as its program — one running application spreading work
		 across cores, NOT a second copy of the process. The caller keeps running.
		 Answers the worker's isolate id, or -1 on failure. Code can't cross the
		 share-nothing boundary, so the worker gets SOURCE (it may define classes
		 and run blocks) plus the shared image; data crosses by copy via send:to:."
		<primitive: IsolateSpawnPrimitive>
		^-1
	]


	class send: anObject to: anIsolateId [
		<primitive: IsolateSendPrimitive>
		^false
	]


	class receive [
		<primitive: IsolateReceivePrimitive>
		^nil
	]
]
