"Stopwatch: a monotonic elapsed-time timer. Because it rides the monotonic
 clock the only firm guarantees are non-negativity and non-decreasing elapsed
 time; the timing checks use a Delay and a generous lower bound so they are not
 flaky on a loaded machine."

[ SwRun := TestRun named: 'Stopwatch'.
  "a fresh stopwatch is not running and reads zero"
  SwRun check: 'new is not running' that: [ Stopwatch new isRunning not ].
  SwRun check: 'new reads zero elapsed' equals: 0 is: [ Stopwatch new elapsedMilliseconds ].
  SwRun check: 'start sets running' that: [ Stopwatch new start isRunning ].
  SwRun check: 'class start returns a running watch' that: [ Stopwatch start isRunning ].
  SwRun check: 'stop clears running' that: [ | w | w := Stopwatch start. w stop. w isRunning not ].
  SwRun check: 'elapsed is a Duration' that: [ Stopwatch new elapsed isKindOf: Duration ].
  SwRun check: 'elapsedNanos non-negative' that: [ Stopwatch new elapsedNanos >= 0 ].

  "elapsed is non-negative and non-decreasing while running"
  SwRun check: 'elapsed non-decreasing' that: [ | w a b |
      w := Stopwatch start.
      a := w elapsedNanos.
      (Delay forMilliseconds: 5) wait.
      b := w elapsedNanos.
      b >= a ].

  "timing a 20 ms delay reads at least 15 ms (generous lower bound)"
  SwRun check: 'millisecondsToRun a 20ms delay >= 15' that: [
      (Stopwatch millisecondsToRun: [ (Delay forMilliseconds: 20) wait ]) >= 15 ].
  SwRun check: 'timeToRun returns a Duration' that: [
      (Stopwatch timeToRun: [ (Delay forMilliseconds: 5) wait ]) isKindOf: Duration ].
  SwRun check: 'timeToRun a 20ms delay >= 15 ms' that: [
      (Stopwatch timeToRun: [ (Delay forMilliseconds: 20) wait ]) asMilliseconds >= 15 ].

  "stop then start again keeps accumulating; reset zeroes it"
  SwRun check: 'reset zeroes a stopped watch' equals: 0 is: [ | w |
      w := Stopwatch start.
      (Delay forMilliseconds: 5) wait.
      w stop.
      w reset.
      w elapsedMilliseconds ].
  SwRun check: 'accumulated across two runs grows' that: [ | w first |
      w := Stopwatch new.
      w start. (Delay forMilliseconds: 10) wait. w stop.
      first := w elapsedNanos.
      w start. (Delay forMilliseconds: 10) wait. w stop.
      w elapsedNanos >= first ].
  SwRun check: 'restart resets and runs' that: [ | w |
      w := Stopwatch start.
      (Delay forMilliseconds: 5) wait.
      w stop.
      w restart.
      w isRunning ].
  ^SwRun report
]
