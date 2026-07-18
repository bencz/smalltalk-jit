"Register-allocator stress: many simultaneously live temps, long chained
 reassignments, and wide arithmetic expressions with several live
 subexpressions. The same computation runs twice: once in a top-level block
 (temps are heap context vars) and once in a real method frame (temps go
 through the linear-scan allocator and its spill paths). Every intermediate
 and final value is checked against hand-computed constants."

RegAllocProbe := Object [

	"Identical computation to the top-level stress block below, but in a
	 method frame. Returns the 14 observed values in a fixed order:
	 initial sum, initial products, a b c d snapshots after their chains,
	 e f g h i (never reassigned), j snapshot, final sum, final products."
	class values [
		| a b c d e f g h i j s1 s2 sa sb sc sd sj s3 s4 out |

		a := 1.
		b := 2.
		c := 3.
		d := 4.
		e := 5.
		f := 6.
		g := 7.
		h := 8.
		i := 9.
		j := 10.

		s1 := a + b + c + d + e + f + g + h + i + j.
		s2 := (a * b) + (c * d) + (e * f) + (g * h) + (i * j).

		a := b.
		a := c.
		a := d.
		a := e.
		a := f.
		a := g.
		a := h.
		a := i.
		a := j.
		sa := a.

		b := a.
		b := c.
		b := d.
		b := e.
		b := f.
		b := g.
		b := h.
		b := i.
		b := j.
		sb := b.

		c := a.
		c := b.
		c := d.
		c := e.
		c := f.
		c := g.
		c := h.
		c := i.
		c := j.
		sc := c.

		d := a.
		d := b.
		d := c.
		d := e.
		d := f.
		d := g.
		d := h.
		d := i.
		d := j.

		d := a.
		d := b.
		d := c.
		d := e.
		d := f.
		d := g.
		d := h.
		d := i.
		d := j.
		sd := d.

		j := a.
		j := b.
		j := c.
		j := d.
		j := e.
		j := f.
		j := g.
		j := h.
		j := i.
		sj := j.

		s3 := a + b + c + d + e + f + g + h + i + j.
		s4 := (a * b) + (c * d) + (e * f) + (g * h) + (i * j).

		out := Array new: 14.
		out at: 1 put: s1.
		out at: 2 put: s2.
		out at: 3 put: sa.
		out at: 4 put: sb.
		out at: 5 put: sc.
		out at: 6 put: sd.
		out at: 7 put: e.
		out at: 8 put: f.
		out at: 9 put: g.
		out at: 10 put: h.
		out at: 11 put: i.
		out at: 12 put: sj.
		out at: 13 put: s3.
		out at: 14 put: s4.
		^out
	]

]


[
	| a b c d e f g h i j s1 s2 sa sb sc sd sj s3 s4 |

	RegAllocRun := TestRun named: 'RegAllocTest'.

	a := 1.
	b := 2.
	c := 3.
	d := 4.
	e := 5.
	f := 6.
	g := 7.
	h := 8.
	i := 9.
	j := 10.

	s1 := a + b + c + d + e + f + g + h + i + j.
	s2 := (a * b) + (c * d) + (e * f) + (g * h) + (i * j).

	a := b.
	a := c.
	a := d.
	a := e.
	a := f.
	a := g.
	a := h.
	a := i.
	a := j.
	sa := a.

	b := a.
	b := c.
	b := d.
	b := e.
	b := f.
	b := g.
	b := h.
	b := i.
	b := j.
	sb := b.

	c := a.
	c := b.
	c := d.
	c := e.
	c := f.
	c := g.
	c := h.
	c := i.
	c := j.
	sc := c.

	d := a.
	d := b.
	d := c.
	d := e.
	d := f.
	d := g.
	d := h.
	d := i.
	d := j.

	d := a.
	d := b.
	d := c.
	d := e.
	d := f.
	d := g.
	d := h.
	d := i.
	d := j.
	sd := d.

	j := a.
	j := b.
	j := c.
	j := d.
	j := e.
	j := f.
	j := g.
	j := h.
	j := i.
	sj := j.

	s3 := a + b + c + d + e + f + g + h + i + j.
	s4 := (a * b) + (c * d) + (e * f) + (g * h) + (i * j).

	RegAllocResults := Array new: 14.
	RegAllocResults at: 1 put: s1.
	RegAllocResults at: 2 put: s2.
	RegAllocResults at: 3 put: sa.
	RegAllocResults at: 4 put: sb.
	RegAllocResults at: 5 put: sc.
	RegAllocResults at: 6 put: sd.
	RegAllocResults at: 7 put: e.
	RegAllocResults at: 8 put: f.
	RegAllocResults at: 9 put: g.
	RegAllocResults at: 10 put: h.
	RegAllocResults at: 11 put: i.
	RegAllocResults at: 12 put: sj.
	RegAllocResults at: 13 put: s3.
	RegAllocResults at: 14 put: s4.
]

[
	"Expected values, computed by hand from the chains above:
	 initial: a..j = 1..10, so sum = 55 and products = 2+12+30+56+90 = 190.
	 a-chain ends a := j, so a = 10; b-chain ends b := j = 10; c-chain ends
	 c := j = 10; both d-chains end d := j = 10; e..i are never reassigned
	 (5 6 7 8 9); j-chain ends j := i = 9.
	 final sum = 10+10+10+10+5+6+7+8+9+9 = 84.
	 final products = (10*10)+(10*10)+(5*6)+(7*8)+(9*9) = 100+100+30+56+81 = 367."

	RegAllocRun check: 'block initial sum' equals: 55 is: [ RegAllocResults at: 1 ].
	RegAllocRun check: 'block initial products' equals: 190 is: [ RegAllocResults at: 2 ].
	RegAllocRun check: 'block a after chain' equals: 10 is: [ RegAllocResults at: 3 ].
	RegAllocRun check: 'block b after chain' equals: 10 is: [ RegAllocResults at: 4 ].
	RegAllocRun check: 'block c after chain' equals: 10 is: [ RegAllocResults at: 5 ].
	RegAllocRun check: 'block d after double chain' equals: 10 is: [ RegAllocResults at: 6 ].
	RegAllocRun check: 'block e untouched' equals: 5 is: [ RegAllocResults at: 7 ].
	RegAllocRun check: 'block f untouched' equals: 6 is: [ RegAllocResults at: 8 ].
	RegAllocRun check: 'block g untouched' equals: 7 is: [ RegAllocResults at: 9 ].
	RegAllocRun check: 'block h untouched' equals: 8 is: [ RegAllocResults at: 10 ].
	RegAllocRun check: 'block i untouched' equals: 9 is: [ RegAllocResults at: 11 ].
	RegAllocRun check: 'block j after chain' equals: 9 is: [ RegAllocResults at: 12 ].
	RegAllocRun check: 'block final sum' equals: 84 is: [ RegAllocResults at: 13 ].
	RegAllocRun check: 'block final products' equals: 367 is: [ RegAllocResults at: 14 ].
]

[
	| mv |

	mv := RegAllocProbe values.
	RegAllocRun check: 'method initial sum' equals: 55 is: [ mv at: 1 ].
	RegAllocRun check: 'method initial products' equals: 190 is: [ mv at: 2 ].
	RegAllocRun check: 'method a after chain' equals: 10 is: [ mv at: 3 ].
	RegAllocRun check: 'method b after chain' equals: 10 is: [ mv at: 4 ].
	RegAllocRun check: 'method c after chain' equals: 10 is: [ mv at: 5 ].
	RegAllocRun check: 'method d after double chain' equals: 10 is: [ mv at: 6 ].
	RegAllocRun check: 'method e untouched' equals: 5 is: [ mv at: 7 ].
	RegAllocRun check: 'method f untouched' equals: 6 is: [ mv at: 8 ].
	RegAllocRun check: 'method g untouched' equals: 7 is: [ mv at: 9 ].
	RegAllocRun check: 'method h untouched' equals: 8 is: [ mv at: 10 ].
	RegAllocRun check: 'method i untouched' equals: 9 is: [ mv at: 11 ].
	RegAllocRun check: 'method j after chain' equals: 9 is: [ mv at: 12 ].
	RegAllocRun check: 'method final sum' equals: 84 is: [ mv at: 13 ].
	RegAllocRun check: 'method final products' equals: 367 is: [ mv at: 14 ].
]

[ ^RegAllocRun report ]
