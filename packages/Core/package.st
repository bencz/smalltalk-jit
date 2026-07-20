"Core package manifest: the kernel of the runtime, as a real package. This
 files: list is the SINGLE SOURCE OF TRUTH for the kernel file set and its
 load order (superclass before subclass). The C bootstrap (vm/tools/
 Bootstrap.c, readCoreManifestFiles) runs before any Smalltalk exists, so it
 reads this manifest with a minimal scanner, not the DSL: keep name: 'Core'
 (the bootstrap validates it) and keep files: one flat #('...' ...) literal
 of simple strings; comments are fine anywhere, including inside the array.
 PackageLoader never loads Core: requires: 'Core' is always satisfied, the
 core IS the image."
PackageSpec new
	name: 'Core';
	version: '0.1.0';
	summary: 'The runtime core: compiler, collections, streams, files, concurrency, exceptions, package tooling';
	files: #(
		'src/Object.st'
		'src/UndefinedObject.st'
		'src/Behavior.st'
		'src/ClassDescription.st'
		'src/MetaClass.st'
		'src/Class.st'
		'src/Boolean.st'
		'src/True.st'
		'src/False.st'
		'src/TypeFeedback.st'
		'src/Magnitudes/Magnitude.st'
		'src/Magnitudes/Number.st'
		'src/Magnitudes/Integer.st'
		'src/Magnitudes/SmallInteger.st'
		'src/Magnitudes/LargeInteger.st'
		'src/Magnitudes/LargePositiveInteger.st'
		'src/Magnitudes/LargeNegativeInteger.st'
		'src/Magnitudes/Fraction.st'
		'src/Magnitudes/ScaledDecimal.st'
		'src/Magnitudes/Float.st'
		'src/Magnitudes/SmallFloat64.st'
		'src/Magnitudes/BoxedFloat64.st'
		'src/Magnitudes/Character.st'
		'src/Magnitudes/DateTime.st'
		'src/Magnitudes/Duration.st'
		'src/Magnitudes/Date.st'
		'src/Magnitudes/Time.st'
		'src/Magnitudes/Stopwatch.st'
		'src/Iterator.st'
		'src/LazySequence.st'
		'src/Collections/Association.st'
		'src/Collections/Collection.st'
		'src/Collections/SequenceableCollection.st'
		'src/Collections/ArrayedCollection.st'
		'src/Collections/Array.st'
		'src/Collections/String.st'
		'src/Collections/Symbol.st'
		'src/Collections/HashedCollection.st'
		'src/Collections/Dictionary.st'
		'src/Collections/Set.st'
		'src/Collections/ByteArray.st'
		'src/Collections/Bag.st'
		'src/Collections/OrderedCollection.st'
		'src/Collections/SortedCollection.st'
		'src/Collections/Interval.st'
		'src/Collections/IdentitySet.st'
		'src/Collections/IdentityDictionary.st'
		'src/Collections/Stack.st'
		'src/Collections/Queue.st'
		'src/Collections/Heap.st'
		'src/Random.st'
		'src/Namespace.st'
		'src/CompiledCode.st'
		'src/CompiledBlock.st'
		'src/CompiledMethod.st'
		'src/Block.st'
		'src/Context.st'
		'src/ContextCopy.st'
		'src/MethodContext.st'
		'src/BlockContext.st'
		'src/ExceptionHandler.st'
		'src/UnwindHandler.st'
		'src/Message.st'
		'src/SourceCode.st'
		'src/FileSourceCode.st'
		'src/Streams/Stream.st'
		'src/Streams/PositionableStream.st'
		'src/Streams/StreamView.st'
		'src/Streams/CollectionStream.st'
		'src/Streams/BufferedStream.st'
		'src/Streams/ExternalStream.st'
		'src/Streams/FileStream.st'
		'src/Streams/Socket.st'
		'src/Streams/ServerSocket.st'
		'src/Streams/InternetAddress.st'

		"The HTTP framework lives outside the core: packages/Std.Http,
		 loaded on demand by the package system."

		'src/GarbageCollector.st'

		"scheduling kernel"
		'src/Processes/ProcessorScheduler.st'
		'src/Processes/Process.st'
		'src/Processes/Delay.st'

		"synchronisation primitives"
		'src/Concurrency/Atomic.st' "generic lock-free atomic cell (load/store/CAS/exchange)"
		'src/Concurrency/AtomicInteger.st' "Atomic subclass: lock-free fetch-add counters"
		'src/Concurrency/AtomicArray.st' "fixed array of atomic slots (per-index CAS/fetch-add)"
		'src/Concurrency/Semaphore.st'
		'src/Concurrency/Channel.st'
		'src/Concurrency/Worker.st'
		'src/Concurrency/SharedDictionary.st' "thread-safe collection for shared state under the pool"
		'src/Concurrency/ConcurrentDictionary.st' "read-mostly, lock-free reads (actor registry / id->pid maps)"

		"The actor framework lives outside the core: packages/Std.Actors."

		'src/Exception.st'
		'src/HandlerEscape.st'
		'src/Error.st'
		"arithmetic error family: ArithmeticError < Error, ZeroDivide < ArithmeticError"
		'src/ArithmeticError.st'
		'src/ZeroDivide.st'
		"minimal system-error classes (kernel adoption is deferred to a later wave)"
		'src/InvalidArgumentError.st'
		'src/NotYetImplementedError.st'
		'src/EmptyCollectionError.st'
		'src/Warning.st'
		'src/MessageNotUnderstood.st'
		'src/OutOfRangeError.st'
		'src/NotFoundError.st'
		'src/ShouldNotImplement.st'
		'src/SubClassResponsibility.st'
		'src/IoError.st'

		'src/Parser/ParseError.st'
		'src/Parser/Parser.st'
		'src/Parser/LiteralNode.st'
		'src/Parser/ClassNode.st'
		'src/Parser/MethodNode.st'
		'src/Parser/BlockNode.st'
		'src/Parser/ExpressionNode.st'
		'src/Parser/MessageExpressionNode.st'
		'src/Parser/VariableNode.st'
		'src/Parser/NilNode.st'
		'src/Parser/TrueNode.st'
		'src/Parser/FalseNode.st'
		'src/Parser/IntegerNode.st'
		'src/Parser/CharacterNode.st'
		'src/Parser/StringNode.st'
		'src/Parser/SymbolNode.st'
		'src/Parser/ArrayNode.st'
		'src/Parser/BlockScope.st'

		'src/Compiler/CompileError.st'
		'src/Compiler/Compiler.st'
		'src/Compiler/InvalidPragmaError.st'
		'src/Compiler/UndefinedVariableError.st'
		'src/Compiler/RedefinitionError.st'
		'src/Compiler/ReadonlyVariableError.st'

		'src/Debugger.st'
		'src/Repl.st'
		'src/Assert.st'
		'src/AssertError.st'
		'src/TestRun.st' "kernel unit-test runner (tests/*.st end with ^t report)"

		"System / file-system utilities: loaded last, after the Streams,
		 DateTime and Random they build on. Class-side wrappers over the OS
		 primitives appended in earlier waves."
		'src/System.st'
		'src/Files/Path.st'
		'src/Files/File.st'
		'src/Files/Directory.st'

		"basic codecs, core citizens: JSON (C fast path in vm/runtime/Json.c,
		 reflective reference in the same .st) and Base64"
		'src/Json.st'
		'src/Base64.st'

		"package system tooling (manifest, loader, CLI driver): core so that
		 st build can run it on top of the base image"
		'src/Packages/PackageRequirement.st'
		'src/Packages/PackageSpec.st'
		'src/Packages/PackageLoader.st'
		'src/Packages/ProjectTool.st');
	testFiles: #();
	yourself
