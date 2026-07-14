cmake_minimum_required(VERSION 3.10)
project(smalltalk-jit)

set(CMAKE_C_FLAGS "-std=gnu11 -pedantic -Werror -Wno-flexible-array-extensions -Wno-c11-extensions -Wno-gnu -fcommon -fno-omit-frame-pointer")

# Default to an optimized build with debug info and assertions kept on (the VM's
# ASSERTs are valuable while the GC/JIT are still maturing). Pass
# -DCMAKE_BUILD_TYPE=Debug for an unoptimized build.
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g")

# Optional sanitizer for debugging memory bugs: -DSANITIZE=address (or undefined).
if(SANITIZE)
	string(APPEND CMAKE_C_FLAGS " -fsanitize=${SANITIZE} -fno-sanitize-recover=all")
	string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=${SANITIZE}")
	string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=${SANITIZE}")
endif()

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
	vm/Fiber.c
	vm/FreeList.c
	vm/GarbageCollector.c
	vm/Handle.c
	vm/Message.c
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
	vm/Safepoint.c
	vm/Scavenger.c
	vm/Scheduler.c
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
target_link_libraries(VM m)
target_link_libraries(st VM)


# Run the whole Smalltalk test suite: `cmake --build <build> --target check`
# (bootstraps a fresh image and runs every tests/*.st).
add_custom_target(check
	COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR} bash ${CMAKE_SOURCE_DIR}/run_tests.sh --no-build
	DEPENDS st
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	USES_TERMINAL
)
