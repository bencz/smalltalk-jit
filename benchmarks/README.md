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

Both print one line of the form `<name> <N> milliseconds`, where N is the TOTAL
for the whole batch. Both verify their own results and raise an error if the
computation is wrong, so a printed number also means the run was correct:
Richards asserts `queuePacketCount = 23246` and `holdCount = 9297`, and
DeltaBlue checks every variable its planner solved for.

### The total, not the mean, and why that mattered

Every benchmark here used to print the mean per iteration, computed with INTEGER
division. That one detail cost this project its wall-clock signal for its whole
recorded history. Richards printed `total // 100`, so it reported 6, 7 or 8 ms
and nothing else, ever; DeltaBlue reported 0 or 1; `ArrayNumericBench` reported
exactly `3` in every row that was ever recorded, and `BigIntBench` exactly `1`.
Meanwhile the instruction counts underneath moved by real amounts. A change
worth several percent was arithmetically incapable of showing up, so
`scripts/report.awk` answered INCONCLUSIVE every single time and the conclusion
"optimisations reduce instructions but do not produce real gains" followed from
the measurement, not from the VM.

Measured properly afterwards, the same tree showed the last change was worth
**-2.6% wall clock on Richards and -1.0% on DeltaBlue**, in the same direction
as its -3.3% instruction count.

So: every benchmark now prints the batch total, and the iteration counts are
scaled so each batch lands somewhere between 120 ms and 650 ms, which puts the
1 ms printing resolution between 0.15% and 0.8%. Where raising the inner
iteration count would have broken a bit-exact check against a C reference
(`MixedArithBench`) or an identity check (`BigIntBench`), the whole verified run
is repeated instead.

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

* **It measures the process, and takes the fastest of several runs.** Each
  recorded datum is the MINIMUM `task-clock` over `ST_AB_REPS` executions
  (default 15). The minimum, not the mean, because the noise on this kind of
  machine is one-sided: interference can only make a run slower. With the mean,
  two disturbed samples out of eight were enough to hide a real 2.6% effect.
  `ms` is that measured wall clock; `selfms` is what the benchmark printed about
  its own timed region, kept because it brackets out VM startup.

* **It refuses to declare a winner it cannot see**, using a SIGN TEST on the
  paired differences. `ab.sh` runs ABBA so drift is common-mode between the two
  labels within a round; run *i* of one label is subtracted from run *i* of the
  other, and the verdict asks how surprising it is that the difference landed on
  the same side that many times. The threshold is the two-sided test at 5%
  (n=8 needs 7 agreeing, n=12 needs 10, n=20 needs 15). A percentile interval
  was tried first and is wrong here: with nearest-rank and n=8, p10 IS the
  minimum and p90 IS the maximum, so "the interval must not cross zero" quietly
  became "not one single pair may disagree".

`benchmarks/results/BASELINE.jsonl` is the committed record. Append to it, do
not rewrite it: the point is to be able to see when a number moved and why.

**WARNING when reading old rows.** The `ms` column changed meaning. Rows without
a `selfms` field predate this and their `ms` is the benchmark's own truncated
integer mean, which is a near-constant and comparable with nothing. Rows WITH
`selfms` carry a real measured process wall clock. Do not plot the two together.
