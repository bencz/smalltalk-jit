smalltalk-jit
=============

A Smalltalk virtual machine written in C. There is no interpreter: bytecode is
only how compiled code is stored, and the JIT turns it into machine code before
anything runs. The language is the Bluebook one, plus a good part of ANSI
Smalltalk and a class definition syntax.

It started as a fork of Ladislav Marek's [yet-another-smalltalk-vm][upstream].
I picked it up in 2020 to learn how a JIT and a garbage collector actually fit
together, and I have been rebuilding pieces of it ever since. The object model
and the compiler are still recognisably his. The JIT backends, the GC work, the
multicore runtime and most of the class library are where my time went.

[upstream]: https://github.com/lm/yet-another-smalltalk-vm

What is in it
-------------

**Three JIT backends.** x86-64 (SysV) is the reference one. ppc64 big-endian
(ELFv1) and ppc64le (ELFv2) are complete and pass the same suite. The big-endian
port targets the base 64-bit PowerPC ISA, so it runs on hardware as old as a G5.
CPU, OS and C ABI are chosen at link time, so a new port is a directory of its
own instead of a spray of ifdefs. `PORTING.md` has the contract.

**A generational GC.** Copying scavenger for the young space, mark and sweep for
the old one, with write barriers, safepoints, and stackmaps emitted by the JIT.

**Multicore.** One shared heap, N OS threads, per-thread allocation buffers, and
collection coordinated by a safepoint handshake.

**Fibers.** Stackful green threads on an epoll scheduler, with growable stacks
that start at 64KB. Actors and a small HTTP server sit on top of them, written
in Smalltalk.

**A class library** that grew a lot: LargeInteger, Fraction and Float, the full
exception protocol (signal:, return:, pass, ensure:, retry), sockets, JSON,
streams, and the usual collections.

Building
--------

Clang or GCC, and CMake. There are no dependencies worth the name: pthreads and
libm, everything else is POSIX. Linux only, for now.

```sh
cmake -S . -B build
cmake --build build -j
LD_LIBRARY_PATH=build build/st -b smalltalk   # compiles the kernel, writes ./snapshot
```

The image is a frozen snapshot, so bootstrap again after touching the C VM or
anything under `smalltalk/`.

Running
-------

```sh
export LD_LIBRARY_PATH=build

build/st                                  # REPL
build/st -e '(1 to: 10) inject: 0 into: [ :a :b | a + b ]'
build/st -f samples/17_mandelbrot.st
```

One quirk to know early: the value of the last top-level block becomes the
process exit code, so the `inject:into:` line above both prints 55 and exits
with 55.

Tests
-----

```sh
./run_tests.sh          # build, bootstrap, run tests/
./run_tests.sh --all    # and every sample
./build.sh              # clean release build plus all of the above
```

41 test files and 36 samples. `samples/README.md` walks through the samples and
lists the syntax quirks worth knowing before writing any Smalltalk against this
VM.

Status
------

A hobby project, and not an attempt at being Pharo. The suite is green on all
three architectures and I still find bugs in it regularly. What I wanted was a
JIT and a GC small enough to hold in your head, and that is what it is.

License
-------

BSD 3-Clause, inherited from the upstream project. See `LICENSE.txt`.

Copyright (c) 2013, Ladislav Marek

Copyright (c) 2020-2026, Alexandre Bencz
