#!/usr/bin/env bash
#
# The growing gate (docs/jit-v2/01-gate.md).
#
# Under the dry cut (ADR 0002) run_tests.sh cannot be green at every commit, so
# it is not the correctness contract. This is. The current level lives in
# scripts/gate.level; this script runs EVERY level up to it, not just the last,
# so a level once reached can never silently regress.
#
#   scripts/gate.sh            run every level up to scripts/gate.level
#   scripts/gate.sh 3          run every level up to 3, ignoring the file
#   scripts/gate.sh --promote  run, and on success record the next level
#
# A level is either passing or failing. There is no partial credit and no
# "known failure" list: the whole point of a gate under a dry cut is that it
# says one thing.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

LEVEL_FILE="scripts/gate.level"
BUILD="${BUILD:-build}"
PROMOTE=0
TARGET=""

for arg in "$@"; do
	case "$arg" in
		--promote) PROMOTE=1 ;;
		[0-9]*) TARGET="$arg" ;;
		*) echo "usage: gate.sh [level] [--promote]"; exit 2 ;;
	esac
done

if [ -z "$TARGET" ]; then
	TARGET="$(cat "$LEVEL_FILE" 2>/dev/null || echo 0)"
fi

if [ -t 1 ]; then
	G=$'\e[32m'; R=$'\e[31m'; B=$'\e[1m'; D=$'\e[2m'; Z=$'\e[0m'
else
	G=""; R=""; B=""; D=""; Z=""
fi

SCRATCH="${TMPDIR:-/tmp}/st-gate-$$"
mkdir -p "$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

# The primitives, for the three levels below that link them BY HAND.
#
# A GLOB and not a list, and that is the point: levels 3, 7 and 8 each link
# Primitive.c, the implementations now live one file per domain beside it, and
# a domain file missing from any one of the three is a link error in a level
# that has nothing to do with the change. That has already broken this gate
# twice in this campaign, the last time for want of -lm when math.h arrived.
# The shell expands this on every run, so a new domain file reaches all three
# by existing. CMakeLists.txt lists them instead, because a glob there is not
# re-evaluated when a file appears.
#
# Deliberately UNQUOTED at every use, so it word-splits and globs.
PRIMITIVE_SOURCES="vm/runtime/Primitive.c vm/runtime/primitives/*.c"

# The domain files that need the FRONT END -- the parser, the compiler and the
# class builder -- and therefore cannot go into the standalone levels.
#
# This is not a build accident, it is the shape of the system. The hand-linked
# levels exist to prove a subsystem with nothing above it (docs/jit-v2/01-gate.md):
# level 3 has no front end at all, level 8 has no compiler, and level 9 brings
# its own stubs for the globals table. Reflect.c is the reflective compiler --
# parsing and class building driven FROM Smalltalk -- so it sits above all
# three, and linking the compiler and the class builder into them to satisfy one
# domain file would dissolve exactly the property they are for.
#
# It is not untested: it is exercised from level 9 (the whole tree compiles)
# upward, by the real build, `st -b` and the package images.
#
# THE NAME IS NARROWER THAN THE LIST, and that is worth reading twice: Process.c
# and Socket.c need the SCHEDULER rather than the front end. What the three have
# in common is the thing that matters -- each sits above a layer these levels
# deliberately do not link, and pulling that layer in to satisfy one domain file
# would dissolve the property the level exists to prove. Socket.c is the clearest
# case: every one of its primitives parks a fiber, so it is the scheduler's
# client by construction.
#
# The table in runtime/Primitive.c names every implemented primitive, so a link
# that omits this file would still have to resolve its symbols; that is what the
# WEAK DEFAULTS there are for, and the reasoning is written at them.
#
# Named rather than silently dropped, and the levels still GLOB, so a new domain
# file is picked up everywhere by existing.
PRIMITIVE_SOURCES_NEEDING_FRONTEND="Reflect.c Process.c Socket.c"

# What the primitives themselves are built on: the numeric tower's C half and
# the closure layout. ONE definition, for the same reason the primitive list has
# one: a level that lists these by hand is a level that breaks the day a new
# domain file uses one of them.
#
# It broke exactly that way. runtime/primitives/LargeInteger.c arrived, reached
# levels 3 and 7 through the glob above, and failed to LINK there because only
# level 8 had listed runtime/BigInt.c -- the kernels it calls. The gate caught
# it, which is the system working; listing them once is what stops the next one.
primitiveSupportSources() {
	printf '%s ' vm/runtime/Number.c vm/runtime/BigInt.c vm/runtime/Closure.c vm/runtime/Json.c vm/runtime/Base64.c
}

# The OS layer the hand-linked levels need. ONE definition, same reason as the
# two lists above: a level that spells these out is a level that breaks the day
# a primitive reaches for an OS call it did not list.
#
# It broke exactly that way. `System arguments`, `System cpuCount` and
# `System randomBytes:` arrived together, and OsCpu.c and OsRandom.c were in
# CMakeLists but in none of the six hand-linked levels, so three of them stopped
# linking at once.
#
# It is the WHOLE linux directory minus the pieces that need a subsystem the low
# levels do not have: OsEvents (the poller) and OsSocket belong to the network
# layer, and OsSignals to the fiber growth handler, which level 1 links on its
# own terms.
osSources() {
	printf '%s ' vm/os/linux/OsFile.c vm/os/linux/OsProcess.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		vm/os/linux/OsCpu.c vm/os/linux/OsRandom.c
}

# The primitives minus the ones above, for the standalone levels.
standalonePrimitiveSources() {
	local file base
	for file in $PRIMITIVE_SOURCES; do
		base="${file##*/}"
		case " $PRIMITIVE_SOURCES_NEEDING_FRONTEND " in
			*" $base "*) continue ;;
		esac
		printf '%s ' "$file"
	done
}

# The flags every hand-linked level compiles with. ONE definition, because nine
# copies of a flag list is nine chances for the gate to be laxer than the real
# build, and the whole point of levels 0 to 8 is that they are the same code
# under the same rules.
#
# -Wall -Wextra -Werror, matching CMakeLists.txt. -Werror alone promotes only
# GCC's default warnings, which leaves out unused-function, unused-parameter,
# sign-compare and missing-field-initializers -- and the last of those is what
# caught main.c leaving classVariableScope implicit, the same field whose
# absence was already silent once.
#
# Deliberately UNQUOTED at every use, so it word-splits.
GATE_CFLAGS="-std=gnu11 -pedantic -Wall -Wextra -Werror -Wno-flexible-array-extensions -g -O1"

fail=0

run_level() {
	local n="$1" name="$2"; shift 2
	printf "  %-2s %-26s " "$n" "$name"
	if out="$("$@" 2>&1)"; then
		echo "${G}pass${Z}"
	else
		echo "${R}FAIL${Z}"
		echo "$out" | tail -20 | sed 's/^/       /'
		fail=$((fail + 1))
	fi
}

# ---- level 0: the heap allocates and collects, with no execution engine -----
# FIRST, not second. Under a dry cut, compiling the whole tree is the HARDEST
# level, not the easiest: half the VM calls into the JIT. The memory subsystem
# depends on no execution engine at all, so it is the first thing that can be
# proved, and it is linked by hand from seven files precisely so it keeps
# passing while the rest of the VM does not yet compile.
level0() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/MemoryTest.c vm/memory/Heap.c vm/memory/Collector.c \
		vm/memory/PageSpace.c vm/memory/Nursery.c vm/memory/RememberedSet.c \
		vm/memory/Roots.c vm/core/ClassTable.c vm/core/Handle.c \
		$(osSources) \
		-o "$SCRATCH/memtest" -lpthread || return 1
	"$SCRATCH/memtest"
}

# ---- level 1: fibers switch, stacks grow, roots are walkable ----------------
# Also standalone, and also before "compila", for the same reason: a context
# switch is either exactly right or it corrupts the machine somewhere else
# entirely, so it is worth proving before anything runs on top of it.
level1() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/FiberTest.c vm/concurrency/Fiber.c vm/concurrency/FiberSwitchX64.c \
		vm/memory/Roots.c vm/os/linux/OsMemory.c \
		-o "$SCRATCH/fibertest" -lpthread || return 1
	"$SCRATCH/fibertest"
}

# ---- level 2: the object layer, still standalone ----------------------------
# Strings, Symbols, Arrays, OrderedCollections built by C code and put through
# collections. The identity checks are the point: a Symbol is interned, so two
# equal Strings must produce the SAME object, and it must still be the same one
# after a collection has moved it. Selector dispatch is a pointer compare, so
# broken interning makes every send in the system silently miss.
level2() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/ObjectTest.c vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c \
		vm/runtime/Dictionary.c vm/core/Smalltalk.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		$(osSources) \
		-o "$SCRATCH/objtest" -lpthread || return 1
	"$SCRATCH/objtest"
}

# ---- level 3: the JIT emits machine code that runs --------------------------
# Hand-written bytecode, compiled to x86-64, executed. The JIT's input is
# bytecode, so writing it by hand is what lets the JIT be proved before anything
# exists that can produce any.
level3() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/JitTest.c vm/jit/Jit.c vm/jit/CompiledMethod.c vm/core/Smalltalk.c \
		vm/jit/MacroAssembler.c vm/jit/Backends.c vm/jit/InlineCache.c \
		$(standalonePrimitiveSources) $(primitiveSupportSources) \
		vm/jit/x64/MacroAssemblerX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		$(osSources) \
		-o "$SCRATCH/jittest" -lpthread -lm || return 1
	"$SCRATCH/jittest"
}

# ---- level 4: bytecode becomes SSA, with deopt state attached ---------------
level4() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/SsaTest.c vm/jit/Ir.c vm/jit/SsaBuild.c \
		-o "$SCRATCH/ssatest" || return 1
	"$SCRATCH/ssatest" >/dev/null
}

# ---- level 5: the optimizer -------------------------------------------------
level5() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/PassTest.c vm/jit/Ir.c vm/jit/Passes.c vm/jit/SsaBuild.c \
		-o "$SCRATCH/passtest" || return 1
	"$SCRATCH/passtest" >/dev/null
}

# ---- level 6: escape analysis and materialization ---------------------------
# Phase 3 of the plan, and its ORDERING is the point: erasing an object is
# correct only if it can be rebuilt when a guard fails. An escape analysis with
# no materialization recipe is not an aggressive optimization, it is a wrong
# answer waiting for a deoptimization.
level6() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/DeoptTest.c vm/jit/Ir.c vm/jit/Passes.c vm/jit/Deopt.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c \
		vm/runtime/Dictionary.c vm/core/Smalltalk.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		$(osSources) \
		-o "$SCRATCH/deopttest" -lpthread || return 1
	"$SCRATCH/deopttest" >/dev/null
}

# ---- level 7: the SSA backend emits machine code that runs ------------------
# THE ORACLE IS TIER 1. Every method is compiled by BOTH code generators, both
# are EXECUTED, and what is checked is that they agree. Under a dry cut
# (ADR 0002) the external oracle is gone and --deopt-stress does not exist yet;
# this is the internal one that does, and it is available precisely because
# tier 1 is a complete, independently proved implementation of the same
# language. A value check would pass on an answer that is right for the wrong
# reason; two code generators arriving at it by different routes would not.
#
# RUN AT SEVERAL REGISTER-POOL SIZES, because ST_SSA_REGS is what makes an
# ordinary method spill. The spill and split paths are the code a twelve
# register file never reaches, so they are the least likely to have executed and
# the most likely to be wrong; at a pool of 2 every method goes through them.
level7() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/BackendTest.c \
		vm/jit/Jit.c vm/jit/CompiledMethod.c vm/jit/MacroAssembler.c \
		vm/jit/Backends.c vm/jit/InlineCache.c vm/core/Smalltalk.c \
		vm/jit/Ir.c vm/jit/SsaBuild.c vm/jit/Passes.c vm/jit/Lir.c \
		vm/jit/Lower.c vm/jit/RegAlloc.c vm/jit/SsaRuntime.c \
		vm/jit/SsaEmitter.c vm/jit/SsaBackends.c vm/jit/SsaBackend.c \
		vm/jit/Specialize.c vm/jit/Tier2Stress.c \
		vm/jit/Deopt.c vm/jit/DeoptResume.c vm/jit/x64/abi/sysv/ResumeSysV.c \
		$(standalonePrimitiveSources) $(primitiveSupportSources) \
		vm/jit/x64/MacroAssemblerX64.c vm/jit/x64/SsaEmitterX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		$(osSources) \
		-o "$SCRATCH/backendtest" -lpthread -lm || return 1
	"$SCRATCH/backendtest" >/dev/null || return 1
	local pool
	for pool in 8 4 2; do
		ST_SSA_REGS=$pool "$SCRATCH/backendtest" >/dev/null || return 1
	done
}


# ---- level 8: primitives, still standalone ----------------------------------
# The other side of a send. Until this level, `3 + 4` compiled to a SEND with
# nothing at the far end of it, so no real program could run.
#
# The discipline being proved here is ADR 0006's, and it is easy to state and
# easy to lose: arithmetic, at:, at:put: and size go through a SEND with an
# inline cache, and are NEVER resolved statically. The old VM resolved them at
# the call site, and when its fast path HIT it jumped over the cache, so the
# profile at exactly the hottest sites recorded only the cases that MISSED.
level8() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/PrimitiveTest.c $(standalonePrimitiveSources) \
		$(primitiveSupportSources) \
		vm/core/Smalltalk.c \
		vm/jit/Jit.c vm/jit/CompiledMethod.c \
		vm/jit/MacroAssembler.c vm/jit/Backends.c vm/jit/InlineCache.c \
		vm/jit/x64/MacroAssemblerX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		$(osSources) \
		-o "$SCRATCH/primtest" -lpthread -lm || return 1
	"$SCRATCH/primtest"
}

# ---- level 9: the front end, source to a running method ---------------------
# The first level whose bytecode is not written by hand. Tokenizer, Parser and
# Ast are the ORIGINAL ones; name resolution and emission are new, because the
# bytecode they targeted is gone.
level9() {
	gcc $GATE_CFLAGS \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/CompileTest.c vm/compiler/Compile.c vm/compiler/Parser.c \
		vm/compiler/Tokenizer.c vm/core/Namespace.c vm/core/Smalltalk.c \
		$(standalonePrimitiveSources) $(primitiveSupportSources) \
		vm/jit/Jit.c vm/jit/CompiledMethod.c \
		vm/jit/MacroAssembler.c vm/jit/Backends.c vm/jit/InlineCache.c \
		vm/jit/x64/MacroAssemblerX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		$(osSources) \
		-o "$SCRATCH/compiletest" -lpthread -lm || return 1
	"$SCRATCH/compiletest"
}

# ---- level 10: the whole tree compiles and links -----------------------------
level10() {
	cmake -S . -B "$BUILD" >/dev/null 2>&1 || return 1
	cmake --build "$BUILD" -j"$(nproc)" >/dev/null 2>&1
}

level11() { "$BUILD/st" -s "$BUILD/gate.img" -e '(3 + 4) printNl' </dev/null | grep -qx 7; }
# Writing an image is only half the level: the criterion is that it RELOADS.
# Three checks, because each one fails for a different reason.
#
#   1. the bootstrap writes an image at all;
#   2. the image loads and the kernel in it RUNS. `printNl` and not `3 + 4`,
#      because a loaded method has no native code and is compiled again on its
#      first send, so printing is what proves the reload reached the JIT;
#   3. save-load-save is a FIXPOINT, byte for byte. This is the only check that
#      catches a field the writer persists and the reader drops: the image still
#      loads and still runs, and the loss shows up nowhere else.
level12() {
	"$BUILD/st" -s "$SCRATCH/gate.img" -b packages/Core </dev/null >/dev/null || return 1
	"$BUILD/st" -s "$SCRATCH/gate.img" -e '(3 + 4) printNl' </dev/null \
		| grep -qx 7 || return 1
	ST_RESAVE="$SCRATCH/gate2.img" "$BUILD/st" -s "$SCRATCH/gate.img" \
		</dev/null >/dev/null || return 1
	cmp -s "$SCRATCH/gate.img" "$SCRATCH/gate2.img"
}
# ---- level 13: the packages build and the project image is generated --------
# The criterion from docs/jit-v2/01-gate.md: the four packages compile and the
# project image is generated. It goes through `st build`, which is the real
# path -- the manifest is evaluated IN the image, the sources are parsed by the
# reflective compiler, and the classes are built into the package's own
# namespace -- so this level exercises the whole of that or none of it.
#
# EVERY .stbuild IS REMOVED FIRST. `st build` answers "up to date" when the
# image is newer than its deps, so a stale image left over from a previous run
# would make this level pass by not building anything, which is the one way a
# build gate can lie.
#
# samples/ is built last and is not one of the four: it REQUIRES the Std.*
# packages by name, so it only builds when the package path resolves and the
# graph loads, which is the end-to-end of the whole arrangement.
level13() {
	rm -rf packages/*/.stbuild samples/.stbuild
	"$BUILD/st" -s "$SCRATCH/core.img" -b packages/Core </dev/null >/dev/null 2>&1 \
		|| return 1
	local project out
	for project in packages/Core packages/Std.Uuid packages/Std.Http \
			packages/Std.Actors samples; do
		out="$( (cd "$project" \
			&& ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SCRATCH/core.img" \
				"$ROOT/$BUILD/st" build) </dev/null 2>&1 )" || {
			echo "$project: $out"; return 1; }
		case "$out" in
			built\ *) ;;
			*) echo "$project did not build: $out"; return 1 ;;
		esac
		[ -f "$project/.stbuild/program.img" ] \
			|| { echo "$project: no program.img was written"; return 1; }
	done
}
level14() { ./run_tests.sh --no-build 2>&1 | grep -q "ALL PASSED"; }
level15() { ./scripts/deopt-stress.sh; }
level16() { echo "level 16 (performance) has no runner yet"; return 1; }

NAMES=("aloca e coleta" "troca de fiber" "objetos e colecoes" "JIT executa" \
       "constroi SSA" "otimiza" "escapa e materializa" "backend de SSA" \
       "primitivas" "front end" "compila" "executa 3 + 4" "bootstrap" \
       "pacotes" "paridade" "deopt-stress" "performance")

echo "${B}gate: running levels 0..$TARGET${Z}"
for n in $(seq 0 "$TARGET"); do
	run_level "$n" "${NAMES[$n]}" "level$n"
done

echo ""
if [ "$fail" -eq 0 ]; then
	echo "${G}${B}gate level $TARGET: PASS${Z}"
	if [ "$PROMOTE" -eq 1 ]; then
		echo $((TARGET + 1)) > "$LEVEL_FILE"
		echo "${D}promoted to level $((TARGET + 1))${Z}"
	fi
	exit 0
fi
echo "${R}${B}gate: $fail level(s) failed${Z}"
exit 1
