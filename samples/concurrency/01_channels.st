"============================================================
  Concurrency 01 - Channels
  ------------------------------------------------------------
  A Channel is an unbounded, blocking queue used to hand values
  between lightweight processes (green threads). Senders never
  block; a receiver blocks until a value is available.

  We build two classic patterns on top of it:
    1. a worker pool that squares numbers in parallel
    2. a producer -> filter -> printer pipeline
  Everything runs cooperatively on a single OS thread.
  ============================================================"

"---- 1. Worker pool -------------------------------------------------------
 Three workers pull jobs from one channel and push results to another. A
 #stop sentinel per worker shuts the pool down cleanly."
[
	| jobs results total |
	jobs := Channel new.
	results := Channel new.

	1 to: 3 do: [:w |
		[ | running |
		  running := true.
		  [running] whileTrue: [
			| job |
			job := jobs receive.
			job = #stop
				ifTrue: [ running := false ]
				ifFalse: [ results send: job * job ] ] ] fork ].

	"feed 8 jobs, then one stop for each worker"
	1 to: 8 do: [:n | jobs send: n].
	1 to: 3 do: [:w | jobs send: #stop].

	"collect the 8 results (order is not guaranteed, the sum is)"
	total := 0.
	1 to: 8 do: [:i | total := total + results receive].
	Transcript nextPutAll: 'worker pool: sum of squares 1..8 = '.
	total printNl.                "204"
]

"---- 2. Pipeline ----------------------------------------------------------
 producer -> evensOnly -> printer, each stage its own process connected by
 channels. A nil marks end-of-stream between stages."
[
	| numbers evens done |
	numbers := Channel new.
	evens := Channel new.
	done := Channel new.

	"stage A: produce 1..10 then signal end with nil"
	[ 1 to: 10 do: [:n | numbers send: n]. numbers send: nil ] fork.

	"stage B: forward only even numbers, propagate the nil"
	[ | running |
	  running := true.
	  [running] whileTrue: [
		| n |
		n := numbers receive.
		n isNil
			ifTrue: [ evens send: nil. running := false ]
			ifFalse: [ n even ifTrue: [ evens send: n ] ] ] ] fork.

	"stage C: collect the evens"
	[ | running collected |
	  running := true.
	  collected := OrderedCollection new.
	  [running] whileTrue: [
		| n |
		n := evens receive.
		n isNil
			ifTrue: [ running := false ]
			ifFalse: [ collected add: n ] ].
	  Transcript nextPutAll: 'pipeline: even numbers 1..10 = '.
	  collected printNl.
	  done send: #finished ] fork.

	"wait for the pipeline to drain"
	done receive.
	Transcript nextPutAll: 'pipeline finished.'; lf.
]
