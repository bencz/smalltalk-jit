"Std.Actors package manifest. ProtoActor-style actor framework (actors,
 supervision, futures, dead letters) on top of the core scheduler and the
 core concurrency primitives (Processor, ConcurrentDictionary,
 AtomicInteger). File order follows the old kernel bootstrap order."
PackageSpec new
	name: 'Std.Actors';
	version: '0.1.0';
	summary: 'Actor framework: actors, supervision, futures, dead letters';
	files: #(
		'src/SystemMessages.st'
		'src/Pid.st'
		'src/Mailbox.st'
		'src/Actor.st'
		'src/Supervision.st'
		'src/Props.st'
		'src/ActorContext.st'
		'src/Future.st'
		'src/DeadLetters.st'
		'src/ActorSystem.st');
	testFiles: #(
		'tests/ActorUnitTest.st'
		'tests/FutureTest.st'
		'tests/ActorTest.st'
		'tests/ActorStressTest.st');
	yourself
