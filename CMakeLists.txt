cmake_minimum_required(VERSION 3.10)
project(smalltalk-jit C)

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

# Execution counters for the jit-v2 work: -DST_INSTRUMENT=1 (vm/core/Instrument.h).
# Off by default and it MUST stay that way: with it off every counter macro
# compiles to nothing and every JIT emission site emits nothing, so an ordinary
# build carries zero instructions of instrumentation. An instrumented build
# emits a memory RMW inside the arithmetic fast paths, which is right for
# counting and useless for timing, so never take a clock reading from one.
if(ST_INSTRUMENT)
	string(APPEND CMAKE_C_FLAGS " -DST_INSTRUMENT=1")
endif()

# Optional sanitizer for debugging memory bugs: -DSANITIZE=address (or undefined).
if(SANITIZE)
	string(APPEND CMAKE_C_FLAGS " -fsanitize=${SANITIZE} -fno-sanitize-recover=all")
	string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=${SANITIZE}")
	string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=${SANITIZE}")
endif()

# ---- CPU architecture selection --------------------------------------------
# ST_ARCH names the vm/jit/<arch>/ directory holding ALL CPU-specific code:
# assembler, code generator, stubs, primitive generators, fiber switch. Arch
# binding is link-time: this block picks WHICH backend .c files get compiled;
# generic code calls them through the arch-neutral vm/jit/Target*.h contract
# headers. Keep this block and vm/jit/TargetAssembler.h in sync (both fail
# loudly on a mismatch). Porting checklist: PORTING.md.
if(NOT ST_ARCH)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
		set(ST_ARCH x64)
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ppc64le|powerpc64le)$")
		set(ST_ARCH ppc64le)
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ppc64|powerpc64)$")
		set(ST_ARCH ppc64)
	else()
		message(FATAL_ERROR
			"No JIT backend for CPU '${CMAKE_SYSTEM_PROCESSOR}'. Supported: x86_64. "
			"To start a port, create vm/jit/<arch>/ and register it here - see PORTING.md.")
	endif()
endif()

if(ST_ARCH STREQUAL "x64")
	# The macro assembler's x64 backend. There is no separate code generator any
	# more: the tier speaks only of frame slots and the backend is the only thing
	# that knows what a register is (ADR 0009).
	set(vmArchSources
		vm/jit/x64/MacroAssemblerX64.c)
	set(vmArchTestSources)
elseif(ST_ARCH STREQUAL "ppc64" OR ST_ARCH STREQUAL "ppc64le")
	# The POWER backend has not been ported to the v2 macro assembler yet. It
	# fails HERE rather than at link time, because a build that gets most of the
	# way and then cannot find a symbol says nothing about why.
	message(FATAL_ERROR
		"The POWER backends are not ported to the jit-v2 macro assembler yet "
		"(vm/jit/MacroAssembler.h). x64 is the only ST_ARCH that builds.")
else()
	message(FATAL_ERROR "Unknown ST_ARCH '${ST_ARCH}' (supported: x64, ppc64, ppc64le)")
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
		vm/os/linux/OsFile.c
		vm/os/linux/OsMemory.c
		vm/os/linux/OsProcess.c
		vm/os/linux/OsRandom.c
		vm/os/linux/OsSignals.c
		vm/os/linux/OsSocket.c
		vm/os/linux/OsThread.c
		vm/os/linux/OsTime.c)
else()
	message(FATAL_ERROR "Unknown ST_OS '${ST_OS}' (supported: linux)")
endif()

# ---- C-ABI selection --------------------------------------------------------
# ST_ABI names the vm/jit/<arch>/abi/<abi>/ directory binding the platform C
# ABI for the selected backend: C argument registers, callee-/caller-saved
# sets, C-call frame shape (shadow space), the PrimitiveResult return
# convention, TLS access and the fiber switch, as an ops-struct (see
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
	elseif(ST_ARCH STREQUAL "ppc64")
		set(ST_ABI elfv1)
	elseif(ST_ARCH STREQUAL "ppc64le")
		set(ST_ABI elfv2)
	else()
		message(FATAL_ERROR
			"No C-ABI binding for (ST_ARCH='${ST_ARCH}', ST_OS='${ST_OS}'). "
			"Create vm/jit/${ST_ARCH}/abi/<abi>/ and register it here - see PORTING.md.")
	endif()
endif()

if(ST_ARCH STREQUAL "x64" AND ST_ABI STREQUAL "sysv")
	# BOTH x64 ABIs are compiled in, not just the host's. The foreign one is what
	# the cross-emission checks read back byte for byte on a machine that cannot
	# execute it, which is what an ops-struct buys and #ifdef does not (ADR 0009).
	set(vmAbiSources
		vm/jit/x64/abi/sysv/AbiSysV.c
		vm/jit/x64/abi/win64/AbiWin64.c)
else()
	message(FATAL_ERROR
		"Unknown ST_ABI '${ST_ABI}' for ST_ARCH '${ST_ARCH}' (implemented: x64+sysv). "
		"The abi/ layout reserves win64 - see PORTING.md.")
endif()

# The jit-v2 source set.
#
# It is SHORTER than the list it replaces, and deliberately: the dry cut (ADR
# 0002) took 87 files out, and what has not been reimplemented yet is not listed
# here rather than being listed and broken. What is still v1 and still on disk:
# core/{CompiledCode,Entry,Exception,Lookup,Namespace,StackFrame}.c,
# compiler/{Compiler,Scope}.c, concurrency/Scheduler.c, runtime/{Base64,
# FileSystem,Iterator,Json,Message,Primitives,Socket,Stream}.c and
# tools/Repl.c. None of them compiles against the new object model,
# and each comes back when its layer does.
set(vmSources
	vm/compiler/Compile.c
	vm/compiler/Parser.c
	vm/compiler/Tokenizer.c
	vm/concurrency/Fiber.c
	vm/concurrency/FiberSwitchX64.c
	vm/core/Class.c
	vm/core/ClassTable.c
	vm/core/Handle.c
	vm/core/Instrument.c
	vm/core/Smalltalk.c
	vm/core/Thread.c
	vm/jit/Backends.c
	vm/jit/CompiledMethod.c
	vm/jit/Deopt.c
	vm/jit/InlineCache.c
	vm/jit/Ir.c
	vm/jit/Jit.c
	vm/jit/MacroAssembler.c
	vm/jit/Passes.c
	vm/jit/SsaBuild.c
	vm/memory/Collector.c
	vm/memory/Heap.c
	vm/memory/Nursery.c
	vm/memory/PageSpace.c
	vm/memory/RememberedSet.c
	vm/memory/Roots.c
	vm/runtime/BigInt.c
	vm/runtime/Closure.c
	vm/runtime/Collection.c
	vm/runtime/Dictionary.c
	vm/runtime/Number.c
	vm/runtime/Primitive.c
	vm/runtime/String.c
	vm/thirdparty/cityhash/city.c
	vm/tools/Bootstrap.c
	vm/tools/ClassBuilder.c
	vm/tools/Snapshot.c
	${vmArchSources}
	${vmAbiSources}
	${vmOsSources}
)

find_package(Threads)
# CityHash byte-swaps its unaligned loads on big-endian ONLY when this macro is
# set (city.c:50-75); without it a BE build computes different, self-consistent
# but untested hashes. Inert on little-endian hosts.
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
	set_source_files_properties(vm/thirdparty/cityhash/city.c
		PROPERTIES COMPILE_DEFINITIONS WORDS_BIGENDIAN)
endif()
add_library(VM SHARED ${vmSources})
add_executable(st main.c)
target_include_directories(st PRIVATE ${CMAKE_SOURCE_DIR})

# All VM-internal includes are bucket-qualified from the vm/ root
# (#include "memory/Heap.h"); vendored third-party code lives under
# vm/thirdparty/ and is included the same way ("thirdparty/cityhash/city.h").
target_include_directories(VM PUBLIC ${CMAKE_SOURCE_DIR}/vm)
# The per-OS include dir is how contract headers reach their impl headers
# (vm/os/OsThread.h includes "OsThreadImpl.h" - the header analogue of the
# ST_OS link-time source selection; OsThread.h #errors when it is missing).
target_include_directories(VM PUBLIC ${CMAKE_SOURCE_DIR}/vm/os/${ST_OS})

target_link_libraries(VM ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(VM m)
target_link_libraries(st VM)

# The GATE (docs/jit-v2/01-gate.md) is NOT built here, and that is deliberate.
# Levels 0 to 8 each link a handful of files BY HAND with no CMake at all,
# precisely so they keep passing while the rest of the tree does not yet
# compile, which is the window the dry cut opened. scripts/gate.sh owns them.
add_custom_target(gate
	COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/gate.sh
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	USES_TERMINAL
)
