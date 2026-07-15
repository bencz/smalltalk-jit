SmallTalk-JIT
========================

… is Smalltalk Virtual Machine. It supports Smalltalk as described in Bluebook
and partially ANSI Smalltalk with support for class definiton syntax.

… is written in C and contains bytecode compiler (used only for compiled code
representation - not interpreted), JIT (currently only x86-64 is supported),
generational GC with moving GC on new space and mark & sweep on old space.

… is tested on x86-64 Linux


Usage
-----

For building VM you need: Clang or GCC and Cmake.

```sh
# within the VM root directory
cmake -S . -B build
cmake --build build -j
LD_LIBRARY_PATH=build build/st -b smalltalk # compiles the kernel, writes ./snapshot
./run_tests.sh --all                        # build + bootstrap + full test suite
```

Source layout
-------------

`vm/` is organized by domain; CPU- and OS-specific code is isolated behind
link-time seams (see `PORTING.md` for the full porting contract):

```
vm/core/         object model + runtime (objects, classes, lookup, threads)
vm/memory/       heap, scavenger (young GC), mark-sweep (old GC), safepoints
vm/compiler/     tokenizer, parser, AST, bytecode compiler, optimizer
vm/jit/          arch-neutral JIT layer (assembler buffer, codegen API,
                 register allocator, stackmaps) + Target*.h arch contracts
vm/jit/x64/      the x86-64 backend (selected by CMake ST_ARCH)
vm/runtime/      built-in classes' C support (collections, strings, streams,
                 sockets, JSON) + the primitives table
vm/concurrency/  fiber scheduler (N workers over one heap) + fibers
vm/os/           the OS seam (vm/os/Os.h contract); one directory per
                 platform (vm/os/linux/ today, selected by CMake ST_OS),
                 split by domain: OsTime, OsMemory, OsEvents, OsSignals, OsCpu
vm/tools/        bootstrap, snapshot, REPL, CLI
vm/tests/        C-level self-test battery (ST_*_TEST env vars) + unit tests
```
