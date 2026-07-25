# Benchmarks

Richards and DeltaBlue, the two classic Smalltalk VM benchmarks, used here as a
regression ruler for the JIT: they are standard, so the numbers are comparable
with other Smalltalk implementations, and they stress the parts of the VM the
`tests/` suite barely touches.

## WARNING: licence

**The two `.st` files in this directory are third-party code under the GNU
General Public License v2, not the BSD licence the rest of this repository uses.**

    Copyright 1996 John Maloney and Mario Wolczko
    Originally distributed with GNU Smalltalk.

They are kept isolated here, with their original headers and a note recording
the changes made (GPL v2 section 2a), and are deliberately NOT part of
`samples/`, which is this project's own BSD-licensed work.

They are Smalltalk *programs the VM runs*, like any other input file. They are
not compiled or linked into `st`, `libVM.so`, or the snapshot image, and no
build target depends on them.

## Running

Build and bootstrap once (see the top-level README), then:

```sh
./run_benchmarks.sh
```

Or individually:

```sh
export LD_LIBRARY_PATH=$PWD/build
./build/st -s snapshot -f benchmarks/Richards.st
./build/st -s snapshot -f benchmarks/DeltaBlue.st
```

Both print one line of the form `<name> <N> milliseconds`, where N is the mean
over 100 iterations. Both verify their own results and raise an error if the
computation is wrong, so a printed number also means the run was correct:
Richards asserts `queuePacketCount = 23246` and `holdCount = 9297`, and
DeltaBlue checks every variable its planner solved for.

Integer division by the iteration count makes the resolution coarse (DeltaBlue
usually reports `1 milliseconds`). That is the original benchmarks' own
reporting, kept as-is so the figures stay comparable; for finer measurements,
raise the iteration count.

## What they cover

| Benchmark | Exercises |
|---|---|
| `Richards.st` | An OS task scheduler: polymorphic message dispatch, deeply nested conditionals, blocks stored and invoked as task bodies, linked-list traversal |
| `DeltaBlue.st` | An incremental constraint solver: allocation and GC churn, class hierarchies, `OrderedCollection` traffic |

## Why they were not running

Both were committed in the very first commit and never ran. They were written
in GNU Smalltalk's dialect, which this VM does not accept: they define classes
as `Super subclass: Name [...]` where this VM wants `Name := Super [...]`, and
the parse failed on line 1 of Richards and line 23 of DeltaBlue. Richards also
used class variables (which this VM does not have, and whose declaration was
commented out in the original anyway) and `Transcript show:` (since added to
`Stream`). Each file's header records exactly what was changed.

Porting them immediately paid for itself: Richards uncovered a real JIT
miscompile, where 5+ nested inlined conditionals exhaust the register pool and
the x64 backend emitted a spilled variable's `SPILLED_REG` (-1) as a register
number. Fixed in `vm/jit/x64/CodeGeneratorX64.c` (`fillVarToReg`), with
`tests/NestedInlinedConditionalTest.st` as the regression. Inlined conditionals
make Richards about 2.7x faster, so the workaround of disabling them
(`ST_NO_INLINE_CF=1`) was never an option.

## Comparing two configurations: `scripts/ab.sh`

`./run_benchmarks.sh` prints one figure per benchmark, which is enough to see
that a benchmark still passes its self-check but not enough to decide whether a
change made anything faster. For that, use the interleaved runner:

```
scripts/ab.sh 'base=' 'notier=ST_NO_TIER=1' 8 > /tmp/run.jsonl
awk -f scripts/report.awk /tmp/run.jsonl
```

Each spec is `LABEL=ENV,ENV`; one spec records a baseline, two compare them.
Set `ST_AB_ROOT_<label>` to compare two BUILDS instead of two configurations,
and `ST_AB_BENCHES` to pick benchmarks.

Three things it does that matter, and why:

* **It interleaves ABBA rather than running all of A then all of B.** Code
  layout in this VM is a lottery: moving a cold block has produced a
  reproducible 2 percent swing with identical executed instructions, and
  changing loop-header alignment from 16 to 32 bytes moved a benchmark 4
  percent. The machine also warms up over a session. Pairing symmetrically
  inside each round cancels monotonic drift instead of handing it to whichever
  side ran first.

* **It records retired instructions, not just wall clock.** On this workload
  instruction counts repeat to a few parts in 10^8, so they answer "did this
  change add work" immediately, while the clock may need many runs to say
  anything. Prefer them, and treat wall clock as confirmation.

* **It refuses to declare a winner it cannot see.** `report.awk` calls wall
  clock INCONCLUSIVE whenever the median difference is smaller than the spread
  within either label. Note that `Richards` and `DeltaBlue` divide their total
  by 100 with integer division, so their wall-clock resolution is 1 ms per 100
  iterations and is close to useless for A/B work; their instruction counts are
  not. `MixedArithBench` reports a single untruncated total for this reason.

`benchmarks/results/BASELINE.jsonl` is the committed record. Append to it, do
not rewrite it: the point is to be able to see when a number moved and why.
