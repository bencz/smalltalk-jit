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
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/MemoryTest.c vm/memory/Heap.c vm/memory/Collector.c \
		vm/memory/PageSpace.c vm/memory/Nursery.c vm/memory/RememberedSet.c \
		vm/memory/Roots.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/os/linux/OsFile.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		-o "$SCRATCH/memtest" -lpthread || return 1
	"$SCRATCH/memtest"
}

# ---- level 1: fibers switch, stacks grow, roots are walkable ----------------
# Also standalone, and also before "compila", for the same reason: a context
# switch is either exactly right or it corrupts the machine somewhere else
# entirely, so it is worth proving before anything runs on top of it.
level1() {
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
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
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/ObjectTest.c vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		vm/os/linux/OsFile.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		-o "$SCRATCH/objtest" -lpthread || return 1
	"$SCRATCH/objtest"
}

# ---- level 3: the JIT emits machine code that runs --------------------------
# Hand-written bytecode, compiled to x86-64, executed. The JIT's input is
# bytecode, so writing it by hand is what lets the JIT be proved before anything
# exists that can produce any.
level3() {
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/JitTest.c vm/jit/Jit.c vm/jit/CompiledMethod.c \
		vm/jit/MacroAssembler.c vm/jit/Backends.c vm/jit/InlineCache.c \
		vm/runtime/Primitive.c vm/runtime/Closure.c \
		vm/jit/x64/MacroAssemblerX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		vm/os/linux/OsFile.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		-o "$SCRATCH/jittest" -lpthread -lm || return 1
	"$SCRATCH/jittest"
}

# ---- level 4: bytecode becomes SSA, with deopt state attached ---------------
level4() {
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/SsaTest.c vm/jit/Ir.c vm/jit/SsaBuild.c \
		-o "$SCRATCH/ssatest" || return 1
	"$SCRATCH/ssatest" >/dev/null
}

# ---- level 5: the optimizer -------------------------------------------------
level5() {
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
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
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/DeoptTest.c vm/jit/Ir.c vm/jit/Passes.c vm/jit/Deopt.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		vm/os/linux/OsFile.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		-o "$SCRATCH/deopttest" -lpthread || return 1
	"$SCRATCH/deopttest" >/dev/null
}

# ---- level 7: primitives, still standalone ----------------------------------
# The other side of a send. Until this level, `3 + 4` compiled to a SEND with
# nothing at the far end of it, so no real program could run.
#
# The discipline being proved here is ADR 0006's, and it is easy to state and
# easy to lose: arithmetic, at:, at:put: and size go through a SEND with an
# inline cache, and are NEVER resolved statically. The old VM resolved them at
# the call site, and when its fast path HIT it jumped over the cache, so the
# profile at exactly the hottest sites recorded only the cases that MISSED.
level7() {
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/PrimitiveTest.c vm/runtime/Primitive.c vm/runtime/Closure.c \
		vm/jit/Jit.c vm/jit/CompiledMethod.c \
		vm/jit/MacroAssembler.c vm/jit/Backends.c vm/jit/InlineCache.c \
		vm/jit/x64/MacroAssemblerX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		vm/os/linux/OsFile.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		-o "$SCRATCH/primtest" -lpthread -lm || return 1
	"$SCRATCH/primtest"
}

# ---- level 8: the front end, source to a running method ---------------------
# The first level whose bytecode is not written by hand. Tokenizer, Parser and
# Ast are the ORIGINAL ones; name resolution and emission are new, because the
# bytecode they targeted is gone.
level8() {
	gcc -std=gnu11 -pedantic -Wno-flexible-array-extensions -g -O1 \
		-Ivm -I. -Ivm/os/linux \
		vm/tests/CompileTest.c vm/compiler/Compile.c vm/compiler/Parser.c \
		vm/compiler/Tokenizer.c \
		vm/runtime/Primitive.c vm/runtime/Number.c vm/runtime/BigInt.c \
		vm/runtime/Closure.c \
		vm/jit/Jit.c vm/jit/CompiledMethod.c \
		vm/jit/MacroAssembler.c vm/jit/Backends.c vm/jit/InlineCache.c \
		vm/jit/x64/MacroAssemblerX64.c \
		vm/jit/x64/abi/sysv/AbiSysV.c vm/jit/x64/abi/win64/AbiWin64.c \
		vm/core/Class.c vm/core/ClassTable.c vm/core/Handle.c \
		vm/runtime/String.c vm/runtime/Collection.c vm/runtime/Dictionary.c \
		vm/memory/Heap.c vm/memory/Collector.c vm/memory/PageSpace.c \
		vm/memory/Nursery.c vm/memory/RememberedSet.c vm/memory/Roots.c \
		vm/os/linux/OsFile.c \
		vm/os/linux/OsMemory.c vm/os/linux/OsThread.c vm/os/linux/OsTime.c \
		-o "$SCRATCH/compiletest" -lpthread -lm || return 1
	"$SCRATCH/compiletest"
}

# ---- level 9: the whole tree compiles and links -----------------------------
level9() {
	cmake -S . -B "$BUILD" >/dev/null 2>&1 || return 1
	cmake --build "$BUILD" -j"$(nproc)" >/dev/null 2>&1
}

level10() { "$BUILD/st" -s "$BUILD/gate.img" -e '(3 + 4) printNl' </dev/null | grep -qx 7; }
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
level11() {
	"$BUILD/st" -s "$SCRATCH/gate.img" -b packages/Core </dev/null >/dev/null || return 1
	"$BUILD/st" -s "$SCRATCH/gate.img" -e '(3 + 4) printNl' </dev/null \
		| grep -qx 7 || return 1
	ST_RESAVE="$SCRATCH/gate2.img" "$BUILD/st" -s "$SCRATCH/gate.img" \
		</dev/null >/dev/null || return 1
	cmp -s "$SCRATCH/gate.img" "$SCRATCH/gate2.img"
}
level12() { echo "level 12 (pacotes) has no runner yet"; return 1; }
level13() { ./run_tests.sh --no-build 2>&1 | grep -q "ALL PASSED"; }
level14() { echo "level 14 (deopt-stress) has no runner yet"; return 1; }
level15() { echo "level 15 (performance) has no runner yet"; return 1; }

NAMES=("aloca e coleta" "troca de fiber" "objetos e colecoes" "JIT executa" \
       "constroi SSA" "otimiza" "escapa e materializa" "primitivas" \
       "front end" "compila" "executa 3 + 4" "bootstrap" "pacotes" \
       "paridade" "deopt-stress" "performance")

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
