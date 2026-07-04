"Regression tests for two VM fixes:
  1. Hot Float loops used to crash (SIGABRT) once ~256 Floats had been
     allocated, because each newFloat leaked a handle into the current
     handle scope. Float arithmetic must now run indefinitely.
  2. Objects larger than a 256 KB heap page could not be allocated in /
     promoted to old space. Large arrays must now work."

"---- A long hot Float loop (far more than 256 allocations) ----"
[
	| z |
	z := 0.0.
	1 to: 20000 do: [:i | z := z + 0.5].
	Assert true: (z - 10000.0) abs < 0.001.
]

"---- Many Floats kept live in an array, then read back (GC survival) ----"
[
	| arr sum |
	arr := Array new: 5000.
	1 to: 5000 do: [:i | arr at: i put: (i asFloat)].
	sum := 0.0.
	arr do: [:x | sum := sum + x].
	"1 + 2 + ... + 5000 = 12502500"
	Assert true: (sum - 12502500.0) abs < 1.0.
]

"---- Float math functions in a loop ----"
[
	| acc |
	acc := 0.0.
	1 to: 1000 do: [:i | acc := acc + (i asFloat sqrt)].
	Assert true: acc > 21000.0.
	Assert true: acc < 21100.0.
]

"---- A single object larger than a 256 KB heap page ----"
[
	| big |
	big := Array new: 50000.        "~400 KB, larger than a page"
	big at: 1 put: 'first'.
	big at: 50000 put: 'last'.
	Assert true: big size = 50000.
	Assert true: (big at: 1) = 'first'.
	Assert true: (big at: 50000) = 'last'.
]
