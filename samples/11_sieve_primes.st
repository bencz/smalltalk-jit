"============================================================
  11 - Sieve of Eratosthenes
  ------------------------------------------------------------
  Find all prime numbers up to a limit by repeatedly crossing
  out the multiples of each prime.
  ============================================================"

[
	| limit isPrime primes |
	limit := 60.

	"isPrime at: i  tells whether i is (still) considered prime."
	isPrime := Array new: limit.
	1 to: limit do: [:i | isPrime at: i put: true].
	isPrime at: 1 put: false.

	"Cross out multiples of every prime up to sqrt(limit)."
	2 to: limit sqrt truncated do: [:i |
		(isPrime at: i) ifTrue: [
			(i * i) to: limit by: i do: [:multiple |
				isPrime at: multiple put: false]]].

	"Gather the survivors."
	primes := OrderedCollection new.
	1 to: limit do: [:i | (isPrime at: i) ifTrue: [primes add: i]].

	Transcript nextPutAll: 'primes up to '; nextPutAll: limit printString;
		nextPutAll: ':'; lf.
	Transcript nextPutAll: '  '. primes printNl.
	Transcript nextPutAll: 'count = '. primes size printNl.
	Transcript nextPutAll: 'sum   = '. primes sum printNl.
]
