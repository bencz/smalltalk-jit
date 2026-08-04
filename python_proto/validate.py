#!/usr/bin/env python3
"""
Bateria de validacao do prototipo.

Cada teste checa uma afirmacao especifica sobre o pipeline, com um
criterio objetivo. O criterio central e o teste de sanidade: o laco de
produto escalar sobre um array de Vec3 tem que compilar para codigo sem
uma unica alocacao e sem um unico acesso tagged dentro do laco.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from stjit.driver import load
from stjit.value import STATS, enc_int, dec_float
from stjit.passes import all_values

HERE = os.path.dirname(os.path.abspath(__file__))
EX = os.path.join(HERE, "examples")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  [%s] %s%s" % ("ok  " if ok else "FALHA", name,
                           ("  -- " + detail) if detail else ""))
    return ok


def rule(title):
    print()
    print("=" * 72)
    print(title)
    print("=" * 72)


def counters(snap, keys=None):
    keys = keys or [k for k in snap if snap[k]]
    return "  ".join("%s=%d" % (k, snap[k]) for k in keys)


def loop_blocks(fn):
    """Blocos que pertencem ao laco mais interno."""
    from stjit.passes import find_loops
    loops, _ = find_loops(fn)
    if not loops:
        return []
    header, body, _ = loops[-1]
    return [b for b in fn.blocks if b.id in body]


TAGGED_OPS_IN_LOOP = {"box_f", "box_i", "unbox_f", "unbox_i", "bool2tag",
                      "tag2bool", "fieldt", "setfieldt", "aload", "astore",
                      "send", "const", "classref"}
ALLOC_OPS = {"newv", "new", "vnew", "anew"}


def analyse_loop(fn):
    blocks = loop_blocks(fn)
    allocs, tagged, total = [], [], 0
    for b in blocks:
        for v in b.instrs:
            total += 1
            if v.op in ALLOC_OPS:
                allocs.append(v.op)
            if v.op in TAGGED_OPS_IN_LOOP:
                tagged.append(v.op)
    return blocks, allocs, tagged, total


# ---------------------------------------------------------------- teste 1

def test_pipeline():
    rule("1. Pipeline completo: layout plano, produto escalar")
    jit = load(os.path.join(EX, "vec3.st"))
    main = jit.new("Main")
    jit.send("setup:", main, enc_int(200))

    jit.send("hotFlat:", main, enc_int(2))          # aquecimento
    m = jit.method("Main", "hotFlat:")

    rec = jit.compile(m)
    st = rec["stats"]
    print("\n  decisoes de inlining:")
    for caller, sel, callee, depth in rec["inlined"]:
        print("    %s%s -> %s" % ("  " * depth, sel, callee))
    if rec["rejected"]:
        print("  rejeitados: %s" % (rec["rejected"],))
    print("\n  passes: guards_removidos=%d  escalarizados=%d  gvn=%d  "
          "pares_box=%d  phis_promovidos=%d  hoisted=%d  mortos=%d"
          % (st.guards_removed, st.scalar_replaced, st.gvn_removed,
             st.box_pairs_removed, st.phis_promoted, st.hoisted,
             st.dead_removed))
    print("  tempo de compilacao: %.1f ms" % (rec["time"] * 1000))

    fn = rec["fn"]
    blocks, allocs, tagged, total = analyse_loop(fn)
    print("\n  corpo do laco interno apos otimizacao:")
    for b in blocks:
        for v in b.instrs:
            print("    %s" % v)

    print()
    check("laco interno sem alocacao (estatico)", not allocs, str(allocs))
    check("laco interno sem operacao tagged (estatico)", not tagged,
          str(tagged))
    check("nenhuma alocacao em toda a funcao", st.allocs_remaining == 0)
    check("nenhum send residual", rec["residual_sends"] == 0)

    # execucao: tier 2
    STATS.reset()
    t = time.time()
    r2 = jit.send("hotFlat:", main, enc_int(50))
    d2 = time.time() - t
    s2 = STATS.snapshot()

    # execucao: tier 0
    m.optimized = None
    STATS.reset()
    t = time.time()
    r1 = jit.send("hotFlat:", main, enc_int(50))
    d1 = time.time() - t
    s1 = STATS.snapshot()

    print()
    print("  %-16s %14s %14s" % ("contador", "tier 0", "tier 2"))
    for k in s1:
        if s1[k] or s2[k]:
            print("  %-16s %14d %14d" % (k, s1[k], s2[k]))
    print("  %-16s %13.3fs %13.3fs   (%.1fx)"
          % ("tempo", d1, d2, d1 / max(d2, 1e-9)))

    print()
    check("resultado identico entre tier 0 e tier 2", r1 == r2,
          "%r vs %r" % (dec_float(r1), dec_float(r2)))
    check("alocacoes em tempo de execucao: 20000 -> 0",
          s1["allocs"] == 20000 and s2["allocs"] == 0,
          "tier0=%d tier2=%d" % (s1["allocs"], s2["allocs"]))
    check("unbox em tempo de execucao zerado",
          s1["unboxes"] > 0 and s2["unboxes"] == 0,
          "tier0=%d tier2=%d" % (s1["unboxes"], s2["unboxes"]))
    check("envios de mensagem eliminados do laco",
          s2["sends"] <= 1, "tier0=%d tier2=%d" % (s1["sends"], s2["sends"]))
    check("tier 2 mais rapido que tier 0", d2 < d1,
          "%.1fx" % (d1 / max(d2, 1e-9)))
    return jit, main


# ---------------------------------------------------------------- teste 2

def test_layout(jit, main):
    rule("2. Efeito do layout: array plano contra array de ponteiros")
    res = {}
    for sel in ("hotFlat:", "hotBoxed:"):
        m = jit.method("Main", sel)
        m.optimized = None
        m.speculation_burned = False
        jit.send(sel, main, enc_int(2))
        rec = jit.compile(m)
        STATS.reset()
        t = time.time()
        r = jit.send(sel, main, enc_int(50))
        dt = time.time() - t
        res[sel] = (r, STATS.snapshot(), dt, rec)

    rf, sf, df, _ = res["hotFlat:"]
    rb, sb, db, _ = res["hotBoxed:"]
    print("  %-22s %14s %14s" % ("", "plano", "ponteiros"))
    for k in ("allocs", "tagged_loads", "field_loads", "guard_checks"):
        print("  %-22s %14d %14d" % (k, sf[k], sb[k]))
    print("  %-22s %13.3fs %13.3fs" % ("tempo", df, db))
    print()
    check("mesmo resultado nos dois layouts", rf == rb)
    check("os dois layouts ficam sem alocacao",
          sf["allocs"] == 0 and sb["allocs"] == 0)
    check("layout plano faz menos acesso tagged",
          sf["tagged_loads"] < sb["tagged_loads"],
          "plano=%d ponteiros=%d" % (sf["tagged_loads"], sb["tagged_loads"]))


# ---------------------------------------------------------------- teste 3

def test_deopt():
    rule("3. Desotimizacao: guard especulativo falhando no meio do laco")
    jit = load(os.path.join(EX, "poly.st"))
    p = jit.new("Poly")
    jit.send("setup:", p, enc_int(200))
    jit.send("sum:", p, enc_int(200))
    m = jit.method("Poly", "sum:")
    rec = jit.compile(m)
    print("  inlinado: %s" % ([c for _, _, c, _ in rec["inlined"]],))

    STATS.reset()
    clean = jit.send("sum:", p, enc_int(200))
    print("  sem veneno: %.1f  (deopts=%d, allocs=%d)"
          % (dec_float(clean), STATS.deopts, STATS.allocs))
    check("caso monomorfico sem desotimizacao", STATS.deopts == 0)
    check("caso monomorfico sem alocacao", STATS.allocs == 0)

    jit.send("poisonAt:", p, enc_int(150))
    esperado = 199 * 14.0 + 5.0
    STATS.reset()
    poisoned = jit.send("sum:", p, enc_int(200))
    print("  com veneno: %.1f  (esperado %.1f, guard_fails=%d, deopts=%d)"
          % (dec_float(poisoned), esperado, STATS.guard_fails, STATS.deopts))
    check("guard detectou a classe inesperada", STATS.guard_fails == 1)
    check("desotimizacao aconteceu", STATS.deopts == 1)
    check("resultado correto apos desotimizar",
          abs(dec_float(poisoned) - esperado) < 1e-9,
          "%r" % dec_float(poisoned))

    print("\n  convergencia por recompilacao:")
    hist = []
    for k in range(6):
        STATS.reset()
        r = jit.send("sum:", p, enc_int(200))
        rc = jit.check_deopt(m)
        hist.append((dec_float(r), STATS.deopts, rc))
        print("    execucao %d: %.1f  deopts=%d%s"
              % (k, dec_float(r), STATS.deopts,
                 "   -> recompilado" if rc else ""))
    check("todas as execucoes corretas",
          all(abs(v - esperado) < 1e-9 for v, _, _ in hist))
    check("houve recompilacao", any(rc for _, _, rc in hist))
    check("desotimizacao para depois de recompilar",
          hist[-1][1] == 0 and hist[-2][1] == 0)
    check("send do sitio queimado virou residual",
          jit.compiled["Poly>>sum:"]["residual_sends"] >= 1)


# ---------------------------------------------------------------- teste 4

def test_materialization():
    """
    Objeto eliminado pela analise de escape mas ainda referenciado por um
    estado de desotimizacao. Ele nao existe no codigo rapido; a receita de
    materializacao tem que reconstrui-lo no momento da falha do guard.
    """
    rule("4. Materializacao de objeto eliminado durante a desotimizacao")
    src = """
Vec3 := Value [
    | x::Float64 y::Float64 z::Float64 |
    x [ ^x ]
    scale: s [ ^Vec3 x: x * s y: y * s z: z * s ]
    norm2 [ ^(x * x) + (y * y) + (z * z) ]
]

Point2 := Value [
    | x::Float64 y::Float64 |
    x [ ^x ]
    scale: s [ ^Point2 x: x * s y: y * s ]
    norm2 [ ^(x * x) + (y * y) ]
]

Runner := Object [
    | items |
    setup: n [
        items := Array new: n.
        0 to: n - 1 do: [ :i | items at: i put: (Vec3 x: 1.0 y: 2.0 z: 3.0) ].
        ^n
    ]
    poisonAt: i [ items at: i put: (Point2 x: 1.0 y: 1.0). ^i ]
    total: n [
        | acc v w u |
        acc := 0.0.
        0 to: n - 1 do: [ :i |
            v := items at: i.
            w := v scale: 2.0.
            u := items at: n - 1 - i.
            acc := acc + w norm2 + u x ].
        ^acc
    ]
]
"""
    from stjit.driver import JIT
    jit = JIT(src)
    r = jit.new("Runner")
    jit.send("setup:", r, enc_int(100))
    jit.send("total:", r, enc_int(100))
    m = jit.method("Runner", "total:")
    rec = jit.compile(m)
    st = rec["stats"]
    mats = 0
    for v in all_values(rec["fn"]):
        if not v.deopt:
            continue
        for fr in v.deopt:
            for sl in fr.slots.values():
                if sl.__class__.__name__ == "Materialize":
                    mats += 1
    print("  objetos escalarizados: %d, receitas de materializacao "
          "em estados de deopt: %d" % (st.scalar_replaced, mats))
    check("houve escalarizacao", st.scalar_replaced > 0)
    check("objeto eliminado tem receita de materializacao", mats > 0)

    STATS.reset()
    clean = jit.send("total:", r, enc_int(100))
    esperado = 100 * 56.0 + 100 * 1.0
    print("  sem veneno: %r (esperado %r, allocs=%d)"
          % (dec_float(clean), esperado, STATS.allocs))
    check("resultado limpo correto",
          abs(dec_float(clean) - esperado) < 1e-9)
    check("sem alocacao no caminho limpo", STATS.allocs == 0)

    # O indice 80 e alcancado primeiro pelo segundo acesso (i = 19), quando
    # `w` ja foi criado e eliminado. A desotimizacao precisa reconstrui-lo.
    jit.send("poisonAt:", r, enc_int(80))
    esperado2 = 99 * 56.0 + 8.0 + 100 * 1.0
    STATS.reset()
    out = jit.send("total:", r, enc_int(100))
    print("  com veneno: %r (esperado %r, guard_fails=%d, deopts=%d)"
          % (dec_float(out), esperado2, STATS.guard_fails, STATS.deopts))
    check("guard falhou com o objeto eliminado vivo", STATS.guard_fails >= 1)
    check("resultado exato apos materializar e desotimizar",
          abs(dec_float(out) - esperado2) < 1e-9,
          "%r vs %r" % (dec_float(out), esperado2))


# ------------------------------------------------------------------- main

def main():
    print("prototipo de JIT para Smalltalk -- bateria de validacao")
    jit, main_obj = test_pipeline()
    test_layout(jit, main_obj)
    test_deopt()
    test_materialization()

    rule("Resumo")
    ok = sum(1 for _, o, _ in RESULTS if o)
    for name, o, detail in RESULTS:
        if not o:
            print("  FALHA: %s  %s" % (name, detail))
    print("  %d de %d verificacoes passaram" % (ok, len(RESULTS)))
    return 0 if ok == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
