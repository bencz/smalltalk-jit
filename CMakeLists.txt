cmake_minimum_required(VERSION 3.10)
project(smalltalk-jit)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# ---- CPU architecture selection --------------------------------------------
# ST_ARCH names the vm/jit/<arch>/ directory holding ALL CPU-specific code:
# assembler, code generator, stubs, primitive generators, fiber switch. Arch
# binding is link-time — this block picks WHICH backend .c files get compiled;
# generic code calls them through the arch-neutral vm/jit/Target*.h contract
# headers. Keep this block and vm/jit/TargetAssembler.h in sync (both fail
# loudly on a mismatch). Porting checklist: PORTING.md.
if(NOT ST_ARCH)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
		set(ST_ARCH x64)
	else()
		message(FATAL_ERROR
			"No JIT backend for CPU '${CMAKE_SYSTEM_PROCESSOR}'. Supported: x86_64. "
			"To start a port, create vm/jit/<arch>/ and register it here - see PORTING.md.")
	endif()
endif()

if(ST_ARCH STREQUAL "x64")
	set(vmArchSources
		vm/jit/x64/Abi.c
		vm/jit/x64/CodeGeneratorX64.c
		vm/jit/x64/PrimitivesX64.c
		vm/jit/x64/StubCodeX64.c)
	# Golden-byte emission tests for this backend's ABI emitters (linked into st).
	set(vmArchTestSources vm/tests/EmitGoldenX64.c)
else()
	message(FATAL_ERROR "Unknown ST_ARCH '${ST_ARCH}' (supported: x64)")
endif()

# ---- OS selection -----------------------------------------------------------
# ST_OS names the vm/os/<os>/ directory implementing the vm/os/Os.h contract,
# split by domain (OsTime/OsMemory/OsEvents/OsSignals/OsCpu, plus future
# domains such as OsCrypto). Same link-time model as ST_ARCH. Porting
# checklist: PORTING.md.
if(NOT ST_OS)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(ST_OS linux)
	else()
		message(FATAL_ERROR
			"No OS layer for '${CMAKE_SYSTEM_NAME}'. Supported: Linux. "
			"To start a port, create vm/os/<os>/ and register it here - see PORTING.md.")
	endif()
endif()

if(ST_OS STREQUAL "linux")
	set(vmOsSources
		vm/os/linux/OsCpu.c
		vm/os/linux/OsEvents.c
		vm/os/linux/OsMemory.c
		vm/os/linux/OsSignals.c
		vm/os/linux/OsTime.c)
else()
	message(FATAL_ERROR "Unknown ST_OS '${ST_OS}' (supported: linux)")
endif()

# ---- C-ABI selection --------------------------------------------------------
# ST_ABI names the vm/jit/<arch>/abi/<abi>/ directory binding the platform C
# ABI for the selected backend: C argument registers, callee-/caller-saved
# sets, C-call frame shape (shadow space), the PrimitiveResult return
# convention, TLS access and the fiber switch — an ops-struct (see
# vm/jit/x64/Abi.h). Derived from (ST_ARCH, ST_OS); same link-time model as
# ST_ARCH/ST_OS. Instance files are compilable on any host of the arch
# (abi-suffixed symbols) so foreign ABIs can be golden-tested; exactly ONE
# Abi<Abi>Bind.c links in and defines the generic names (gX64Abi, the
# TargetFiber.h symbols). Porting checklist: PORTING.md.
if(NOT ST_ABI)
	if(ST_ARCH STREQUAL "x64" AND (ST_OS STREQUAL "linux" OR ST_OS STREQUAL "osx"))
		set(ST_ABI sysv)
	elseif(ST_ARCH STREQUAL "x64" AND ST_OS STREQUAL "windows")
		set(ST_ABI win64)
	else()
		message(FATAL_ERROR
			"No C-ABI binding for (ST_ARCH='${ST_ARCH}', ST_OS='${ST_OS}'). "
			"Create vm/jit/${ST_ARCH}/abi/<abi>/ and register it here - see PORTING.md.")
	endif()
endif()

if(ST_ARCH STREQUAL "x64" AND ST_ABI STREQUAL "sysv")
	set(vmAbiSources
		vm/jit/x64/abi/sysv/AbiSysV.c
		vm/jit/x64/abi/sysv/AbiSysVBind.c
		vm/jit/x64/abi/sysv/FiberSysV.c)
else()
	message(FATAL_ERROR
		"Unknown ST_ABI '${ST_ABI}' for ST_ARCH '${ST_ARCH}' (implemented: x64+sysv). "
		"The abi/ layout reserves win64 (and ppc64's elfv1/elfv2) - see PORTING.md.")
endif()

set(vmSources
	vm/compiler/Compiler.c
	vm/compiler/Optimizer.c
	vm/compiler/Parser.c
	vm/compiler/Scope.c
	vm/compiler/Tokenizer.c
	vm/concurrency/Fiber.c
	vm/concurrency/Scheduler.c
	vm/core/Class.c
	vm/core/CompiledCode.c
	vm/core/Entry.c
	vm/core/Exception.c
	vm/core/Handle.c
	vm/core/Lookup.c
	vm/core/Smalltalk.c
	vm/core/StackFrame.c
	vm/core/Thread.c
	vm/jit/RegisterAllocator.c
	vm/memory/FreeList.c
	vm/memory/GarbageCollector.c
	vm/memory/Heap.c
	vm/memory/HeapPage.c
	vm/memory/Safepoint.c
	vm/memory/Scavenger.c
	vm/runtime/Collection.c
	vm/runtime/Dictionary.c
	vm/runtime/Iterator.c
	vm/runtime/Json.c
	vm/runtime/Message.c
	vm/runtime/Primitives.c
	vm/runtime/Socket.c
	vm/runtime/Stream.c
	vm/runtime/String.c
	vm/tools/Bootstrap.c
	vm/tools/Repl.c
	vm/tools/Snapshot.c
	${vmArchSources}
	${vmAbiSources}
	${vmOsSources}
)

find_package(Threads)
add_library(CityHash SHARED cityhash/city.c)
add_library(Linenoise SHARED linenoise/linenoise.c)
add_library(VM SHARED ${vmSources})
add_executable(st main.c vm/tests/SelfTests.c ${vmArchTestSources})
target_include_directories(st PRIVATE ${CMAKE_SOURCE_DIR})

# Standalone C unit test for the tokenizer (own main(), not part of `st`):
# built on demand — `cmake --build <build> --target TokenizerTest`.
# (vm/tests/ParserTest.c is NOT wired: it predates the current AST API and no
# longer compiles — candidate for deletion.)
add_executable(TokenizerTest EXCLUDE_FROM_ALL vm/tests/TokenizerTest.c)
target_link_libraries(TokenizerTest VM)

# All VM-internal includes are bucket-qualified from the vm/ root
# (#include "memory/Heap.h"); third-party includes are root-qualified
# (#include "cityhash/city.h").
target_include_directories(VM PUBLIC ${CMAKE_SOURCE_DIR}/vm)
target_include_directories(VM PRIVATE ${CMAKE_SOURCE_DIR})

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
