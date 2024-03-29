cmake_minimum_required(VERSION 2.8)
project(smalltalk-jit)

set(CMAKE_C_FLAGS "-std=gnu11 -pedantic -Wno-flexible-array-extensions -Wno-c11-extensions -Wno-gnu -fcommon")

set(vmSources
	vm/Bootstrap.c
	vm/Class.c
	vm/CodeGeneratorX64.c
	vm/Collection.c
	vm/CompiledCode.c
	vm/Compiler.c
	vm/Dictionary.c
	vm/Entry.c
	vm/Exception.c
	vm/FreeList.c
	vm/GarbageCollector.c
	vm/Handle.c
	vm/Heap.c
	vm/HeapPage.c
	vm/Iterator.c
	vm/Lookup.c
	vm/Optimizer.c
	vm/OsLinux.c
	vm/Parser.c
	vm/Primitives.c
	vm/RegisterAllocator.c
	vm/Repl.c
	vm/Scavenger.c
	vm/Scope.c
	vm/Smalltalk.c
	vm/Snapshot.c
	vm/Socket.c
	vm/StackFrame.c
	vm/Stream.c
	vm/String.c
	vm/StubCodeX64.c
	vm/Thread.c
	vm/Tokenizer.c
)

find_package(Threads)
add_library(CityHash SHARED cityhash/city.c)
add_library(Linenoise SHARED linenoise/linenoise.c)
add_library(VM SHARED ${vmSources})
add_executable(st main.c)

target_link_libraries(VM CityHash)
target_link_libraries(VM Linenoise)
target_link_libraries(VM ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(st VM)
