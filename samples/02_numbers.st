"============================================================
  02 - The Numeric Tower
  ------------------------------------------------------------
  SmallInteger, LargeInteger, Fraction and Float all cooperate
  through automatic coercion. Integer arithmetic never silently
  overflows: it promotes to LargeInteger instead.
  ============================================================"

"---- SmallInteger arithmetic ----"
[
	Transcript nextPutAll: '=== Integer arithmetic ==='; lf.
	Transcript nextPutAll: '2 + 3 * 4      = '.  (2 + 3 * 4) printNl.   "left-to-right: (2+3)*4 = 20"
	Transcript nextPutAll: '17 // 5        = '.  (17 // 5) printNl.     "floored quotient"
	Transcript nextPutAll: '17 \\ 5        = '.  (17 \\ 5) printNl.     "modulo"
	Transcript nextPutAll: '2 raisedTo: 10 = '.  (2 raisedTo: 10) printNl.
	Transcript nextPutAll: '48 gcd: 36     = '.  (48 gcd: 36) printNl.
	Transcript nextPutAll: '4 lcm: 6       = '.  (4 lcm: 6) printNl.
]

"---- LargeInteger: no overflow, arbitrary precision ----"
[
	Transcript nextPutAll: '=== Big integers ==='; lf.
	Transcript nextPutAll: '30 factorial   = '.  30 factorial printNl.
	Transcript nextPutAll: '2 raisedTo: 128 = '. (2 raisedTo: 128) printNl.
]

"---- Fraction: exact rational arithmetic ----"
[
	Transcript nextPutAll: '=== Fractions ==='; lf.
	Transcript nextPutAll: '1/2 + 1/3      = '.  (1/2 + (1/3)) printNl.
	Transcript nextPutAll: '3/4 * 8        = '.  (3/4 * 8) printNl.       "reduces to whole number"
	Transcript nextPutAll: '(22/7) asFloat = '.  (22/7) asFloat printNl.
]

"---- Float: IEEE-754 doubles and math functions ----"
[
	Transcript nextPutAll: '=== Floats ==='; lf.
	Transcript nextPutAll: 'Float pi       = '.  Float pi printNl.
	Transcript nextPutAll: '2 sqrt         = '.  2 sqrt printNl.
	Transcript nextPutAll: '1 exp          = '.  1 exp printNl.
	Transcript nextPutAll: '100 ln         = '.  100 ln printNl.
	Transcript nextPutAll: '(Float pi) sin = '.  Float pi sin printNl.
]

"---- Radix / bases ----"
[
	Transcript nextPutAll: '=== Number bases ==='; lf.
	Transcript nextPutAll: '255 in base 2  = '.  (255 printStringBase: 2) printNl.
	Transcript nextPutAll: '255 in base 16 = '.  (255 printStringBase: 16) printNl.
	Transcript nextPutAll: '16rFF          = '.  16rFF printNl.
	Transcript nextPutAll: '2r1010         = '.  2r1010 printNl.
]
