# Do optional type annotations pay for themselves here?

This is the M11 measurement gate. It exists to answer one question with counters
instead of opinion, before any syntax, parser work or snapshot bump:

> If a method could declare the class of its parameters, how much of this VM's
> dispatch would actually disappear?

The answer, measured, is **about one percent of hot dynamic send sites**. The
rest of this file is how that number was produced and why it is the number that
matters.

## Running it

    ST_TYPE_STATS=1 ./build/st -s snapshot -f benchmarks/Richards.st

Lower `ST_TIER_THRESHOLD` to widen the hot sample (the HOT scope only sees
methods that reached tier 1). The counters live in `vm/jit/Tier.h`; the ALL
scope is counted in `tierMethodHasDynamicSend`, the HOT scope in
`vm/compiler/Optimizer.c`, both using the same send classification the backends
use (`vm/jit/SendClassify.h`), so the census can never disagree with the
decision the compiler actually made.

## What is counted

Two scopes, because they answer different questions.

- **ALL**: every framed method the tier-0 codegen compiles, once each. The shape
  of the source as written.
- **HOT**: the same census over methods that reached tier 1. Reaching tier 1 is
  the hotness filter, so this is the decision-relevant scope.

Neither weights a site by how often it EXECUTES: a site in a loop body counts
once. That limitation cuts in a known direction, see the caveat at the end.

Dynamic sends are split by what the receiver operand is. `self` is broken out
separately from the other arguments on purpose: in the bytecode `self` is an
`OPERAND_ARG_VAR` like any other (`Scope.c` defines it at `SELF_INDEX`, ahead of
the real parameters), and counting it as an annotatable argument is the single
easiest way to overstate this whole measurement. A self-send's receiver is
whatever subclass is running the method; no annotation on that method can pin
it.

## The result

Corpus: all of `tests/`, `samples/` and `benchmarks/`, `ST_TIER_THRESHOLD=100`,
summed. 10599 compiled methods, 54076 sends, 701 methods reaching tier 1.

Where the sends go, hot scope:

| receiver | share of hot dynamic sends |
|---|---|
| temp | 44.6% |
| self | 21.7% |
| instVar | 14.2% |
| **arg** | **11.9%** |
| other (globals, assoc) | 6.7% |
| contextVar | 0.9% |

The first observation is that "annotate the parameters" reaches 11.9% of hot
dynamic sends, not a majority. The big bucket is local temporaries.

Now the part that decides it. Over the annotatable sites only (hot receivers
that are an arg or a temp, 1809 of them), what would a declared exact class
actually buy?

| verdict | sites | share |
|---|---|---|
| cold, never ran | 1267 | 70.0% |
| mono | 421 | 23.3% |
| pic | 88 | 4.9% |
| cold but did run | 25 | 1.4% |
| mega | 8 | 0.4% |

Read that as three groups, because "the compiler would learn something" and
"the program would get faster" are not the same group, and conflating them is
how a type system gets built for nothing:

- **70% never executed.** A declared class there is genuinely new information
  and saves exactly zero dispatches, because the site does not run. It buys
  type CHECKING, which is a correctness feature, not the performance argument.
- **23% are already monomorphic**, which means the tier already devirtualized
  them: promoted to a guarded direct call, or inlined outright. What is left to
  win is the exact-class guard, 2 to 5 instructions on a call that is already
  direct. Of the annotatable sites that ACTUALLY RAN, **77.7% are in this
  state.**
- **4.9% are polymorphic.** An exact declared type at a PIC site is not an
  optimization, it is a program that raises.

Sites where an annotation would remove a REAL dispatch, that is mega plus the
cold cells whose witness proves they ran: **33 out of 1809 annotatable (1.8%),
and 33 out of 3202 hot dynamic sends (1.0%).**

Richards and DeltaBlue alone, run individually, agree: 0.0% dispatch removal,
and 100% / 84.8% of the annotatable sites that ran were already monomorphic.

## Why "cold" had to be split

An unlinked IC cell at recompile time means one of two very different things,
and the whole reading hinges on telling them apart:

- the site **never ran**, or
- the site ran and the **scavenge wiped the cell**. `icResetNativeCodeCells`
  runs at every scavenge, and a long run resets each cell many times over: this
  corpus showed 15 reset sweeps and 1905 cells reset over 460 sites in Richards
  alone, so this was not a hypothetical.

If cold were dominated by GC resets, the cheap fix would be persistent type
feedback (snapshotting `(class, target)` pairs into `NativeCode.typeFeedback`
before the reset sweep), not a type system. So the census records a witness:
reaching `inlineCacheMiss` proves the site executed, and `typeStatsNoteExecuted`
records the cell address there. Measured, the GC-wiped bucket is essentially
empty, so the cold sites are genuinely untaken paths inside hot methods, error
arms and rare cases. That is what makes the 1% figure trustworthy rather than an
artifact of collector timing.

The dump prints `sites seen executing` as a sanity line: if that is zero the
witness never fired and the cold split means nothing.

## Caveat, and which way it cuts

This is a census of SITES, not of executions. One site in the hottest loop could
in principle dominate all of them.

That caveat makes the verdict stronger, not weaker. The hotter a site is, the
more likely it is bound and monomorphic, and monomorphic is exactly the bucket
where the tier has already removed the dispatch. Weighting by execution count
would move sites out of "cold, never ran" and into "already mono", which is the
bucket where an annotation buys a guard rather than a dispatch. It cannot
manufacture dispatches that the inline caches did not already eliminate.

## Verdict

The performance case for optional type annotations, as an argument about
removing dispatch, does not survive contact with this VM's own numbers. The
inline caches plus the tier-1 promotion already devirtualize essentially
everything hot; there is close to nothing left for a declared type to remove.

This does not say type annotations are worthless. It says the reason to build
them would be **checking**, documentation and tooling, and that reason should be
argued on its own terms rather than on a speed claim this census does not
support. The plan's half-day experiment (hardcoding a `RawClass *` return in
`compiledCodeResolveOperandClass` for one hot operand and measuring) remains the
way to falsify this from the other direction, and it is now cheap to interpret:
it should move nothing.
