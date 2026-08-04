"Programa de validacao.

 Dois lacos identicos em semantica, diferentes apenas no layout dos dados:
 `hotFlat:` percorre um ValueArray(Vec3), com os campos embutidos e
 contiguos; `hotBoxed:` percorre um Array generico de ponteiros para Vec3.
 Depois de otimizados, ambos devem ficar sem alocacao. O que sobra de
 diferenca entre eles e exatamente o custo do layout."

Vec3 := Value [
    | x::Float64 y::Float64 z::Float64 |

    x [ ^x ]
    y [ ^y ]
    z [ ^z ]

    dot: o [
        ^(x * o x) + (y * o y) + (z * o z)
    ]

    scale: s [
        ^Vec3 x: x * s y: y * s z: z * s
    ]
]

Main := Object [
    | flat boxed n |

    setup: count [
        n := count.
        flat := Vec3 arrayNew: count.
        boxed := Array new: count.
        0 to: count - 1 do: [ :i |
            flat at: i put: (Vec3 x: i asFloat
                                  y: i asFloat + 1.0
                                  z: i asFloat * 0.5).
            boxed at: i put: (Vec3 x: i asFloat
                                   y: i asFloat + 1.0
                                   z: i asFloat * 0.5) ].
        ^count
    ]

    hotFlat: iters [
        | total v w |
        total := 0.0.
        1 to: iters do: [ :k |
            0 to: n - 1 do: [ :i |
                v := flat at: i.
                w := v scale: 0.5.
                total := total + (v dot: w) ] ].
        ^total
    ]

    hotBoxed: iters [
        | total v w |
        total := 0.0.
        1 to: iters do: [ :k |
            0 to: n - 1 do: [ :i |
                v := boxed at: i.
                w := v scale: 0.5.
                total := total + (v dot: w) ] ].
        ^total
    ]
]
