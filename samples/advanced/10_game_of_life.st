"============================================================
  ADVANCED - Conway's Game of Life
  ------------------------------------------------------------
  A cellular automaton on a grid of cells. Each generation, a
  cell lives or dies based on its 8 neighbours:

    * a live cell with 2 or 3 live neighbours survives
    * a dead cell with exactly 3 live neighbours is born
    * everything else dies or stays dead

  We seed a 'glider' and watch it walk across the board.
  Everything is integer arithmetic - no floating point.
  ============================================================"

GameOfLife := Object [
	| rows cols cells |

	class rows: r cols: c [ ^self new setRows: r cols: c ]

	setRows: r cols: c [
		rows := r.
		cols := c.
		cells := self emptyGrid.
	]

	emptyGrid [
		| grid |
		grid := Array new: rows.
		1 to: rows do: [:i |
			| row |
			row := Array new: cols.
			1 to: cols do: [:j | row at: j put: 0].
			grid at: i put: row].
		^grid
	]

	at: r col: c [
		"Cells outside the grid count as dead."
		(r < 1 or: [r > rows or: [c < 1 or: [c > cols]]]) ifTrue: [^0].
		^(cells at: r) at: c
	]

	at: r col: c put: value [ (cells at: r) at: c put: value ]

	liveNeighboursAt: r col: c [
		| total |
		total := 0.
		-1 to: 1 do: [:dr |
			-1 to: 1 do: [:dc |
				(dr = 0 and: [dc = 0]) ifFalse: [
					total := total + (self at: r + dr col: c + dc)]]].
		^total
	]

	step [
		| next |
		next := self emptyGrid.
		1 to: rows do: [:r |
			1 to: cols do: [:c |
				| n alive |
				n := self liveNeighboursAt: r col: c.
				alive := (self at: r col: c) = 1.
				((alive and: [n = 2 or: [n = 3]]) or: [alive not and: [n = 3]])
					ifTrue: [(next at: r) at: c put: 1]]].
		cells := next.
	]

	draw [
		1 to: rows do: [:r |
			1 to: cols do: [:c |
				Transcript nextPut: ((self at: r col: c) = 1 ifTrue: [$#] ifFalse: [$.])].
			Transcript lf].
	]
]

[
	| life |
	life := GameOfLife rows: 10 cols: 20.

	"Seed a glider in the top-left corner."
	life at: 1 col: 2 put: 1.
	life at: 2 col: 3 put: 1.
	life at: 3 col: 1 put: 1.
	life at: 3 col: 2 put: 1.
	life at: 3 col: 3 put: 1.

	0 to: 6 do: [:generation |
		Transcript nextPutAll: '-- generation '; nextPutAll: generation printString;
			nextPutAll: ' --'; lf.
		life draw.
		life step].
]
