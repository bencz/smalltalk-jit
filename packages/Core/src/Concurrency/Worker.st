"Run blocks in PARALLEL across OS threads, all sharing THIS heap (unlike Isolate,
 which is share-nothing). `Worker parallel:` takes an Array of 0-argument blocks,
 runs each on its own worker OS thread, waits for all of them, and answers an Array
 of their results (result i = (blocks at: i) value). Garbage collection across the
 workers is coordinated by a stop-the-world safepoint, so the workers may allocate.

 Best for CPU-bound work. The blocks run truly in parallel on separate cores; the
 caller blocks until every worker finishes (a parallel barrier)."
Worker := Object [

	class parallel: anArrayOfBlocks [
		<primitive: WorkerParallelPrimitive>
		^self error: 'WorkerParallelPrimitive failed'
	]
]
