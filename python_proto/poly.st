"Caso de desotimizacao: o laco e monomorfico durante o aquecimento, o
 otimizador especula em cima disso, e depois um objeto de outra classe
 aparece no meio da iteracao."

Vec3 := Value [
    | x::Float64 y::Float64 z::Float64 |
    x [ ^x ]
    y [ ^y ]
    z [ ^z ]
    dot: o [ ^(x * o x) + (y * o y) + (z * o z) ]
]

Point2 := Value [
    | x::Float64 y::Float64 |
    x [ ^x ]
    y [ ^y ]
    dot: o [ ^(x * o x) + (y * o y) ]
]

Poly := Object [
    | items |

    setup: n [
        items := Array new: n.
        0 to: n - 1 do: [ :i |
            items at: i put: (Vec3 x: 1.0 y: 2.0 z: 3.0) ].
        ^n
    ]

    poisonAt: idx [
        items at: idx put: (Point2 x: 1.0 y: 2.0).
        ^idx
    ]

    sum: n [
        | total v |
        total := 0.0.
        0 to: n - 1 do: [ :i |
            v := items at: i.
            total := total + (v dot: v) ].
        ^total
    ]
]
