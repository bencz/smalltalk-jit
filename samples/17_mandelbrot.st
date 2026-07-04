"============================================================
  17 - Mandelbrot Set (ASCII art)
  ------------------------------------------------------------
  For each point c in the complex plane we iterate
  z := z*z + c  starting from z = 0, and count how many steps
  it takes for |z| to escape beyond 2. Points that never escape
  belong to the set.

  To keep the inner loop fast and allocation-free we use
  fixed-point integer arithmetic: every real number r is stored
  as the integer  round(r * scale). A product of two fixed-point
  numbers is divided by 'scale' to rescale it.
  ============================================================"

[
	| cols rows maxIter scale palette |
	cols := 74.
	rows := 34.
	maxIter := 50.
	scale := 1000.
	palette := ' .:-=+*oO#'.

	0 to: rows - 1 do: [:row |
		0 to: cols - 1 do: [:col |
			| cRe cIm zRe zIm iter escaped |
			"Map the grid onto the region [-2.5, 1.0] x [-1.15, 1.15]."
			cRe := (col * 3500 // cols) - 2500.
			cIm := (row * 2300 // rows) - 1150.
			zRe := 0.
			zIm := 0.
			iter := 0.
			escaped := false.
			1 to: maxIter do: [:step |
				escaped ifFalse: [
					| re2 im2 |
					re2 := zRe * zRe // scale.
					im2 := zIm * zIm // scale.
					(re2 + im2 > (4 * scale))
						ifTrue: [escaped := true]
						ifFalse: [
							| newRe |
							newRe := re2 - im2 + cRe.
							zIm := (2 * zRe * zIm // scale) + cIm.
							zRe := newRe.
							iter := iter + 1]]].
			escaped
				ifTrue:  [Transcript nextPut: (palette at: (iter * palette size // maxIter) + 1)]
				ifFalse: [Transcript nextPut: $#]].
		Transcript lf].
]
