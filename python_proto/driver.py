"""
Tiering: quando compilar, quando recompilar, quando desistir.

Politica simples e suficiente para o prototipo:

  - tier 0 sempre, ate acumular perfil;
  - promocao para tier 2 por contador de chamadas ou de backedge;
  - apos N desotimizacoes no mesmo metodo, joga fora o codigo e recompila
    com o perfil atualizado. Como o cache inline agora esta polimorfico,
    o inlining especulativo naquele sitio simplesmente nao acontece de
    novo, e o send fica residual. E assim que um sistema real converge
    em vez de ficar oscilando entre otimizar e desotimizar.
"""

import time

from .front import parse
from .compile import compile_program
from .runtime import Runtime
from .lower import build_function
from .passes import optimize
from .backend import compile_tier2

CALL_THRESHOLD = 8
BACKEDGE_THRESHOLD = 2000
DEOPT_GIVEUP = 3


class JIT:
    def __init__(self, source):
        self.ct = compile_program(parse(source))
        self.rt = Runtime(self.ct)
        self.compiled = {}
        self.log = []

    # ------------------------------------------------------------ api
    def new(self, class_name, *args):
        c = self.ct.find(class_name)
        return self.rt.instantiate(c.index, list(args))

    def send(self, sel, recv, *args):
        return self.rt.send(sel, recv, tuple(args))

    def method(self, class_name, sel):
        return self.ct.find(class_name).methods[sel]

    # ------------------------------------------------------ compilacao
    def compile(self, method, verbose=False):
        t0 = time.time()
        fn = build_function(self.rt, method)
        st = optimize(fn)
        code = compile_tier2(self.rt, fn, method)
        method.optimized = code
        dt = time.time() - t0
        rec = {
            "method": method.full_name(),
            "time": dt,
            "inlined": list(fn.inline_stats.inlined),
            "rejected": list(fn.inline_stats.rejected),
            "residual_sends": fn.residual_sends,
            "guards_emitted": fn.inline_stats.guards,
            "stats": st,
            "fn": fn,
            "code": code,
        }
        self.compiled[method.full_name()] = rec
        self.log.append(rec)
        if verbose:
            print("[jit] %s compilado em %.1f ms" % (method.full_name(),
                                                     dt * 1000))
        return rec

    def maybe_compile(self, method, verbose=False):
        if method.optimized is not None:
            return None
        if (method.calls >= CALL_THRESHOLD
                or method.loop_backedges >= BACKEDGE_THRESHOLD):
            return self.compile(method, verbose)
        return None

    def check_deopt(self, method, verbose=False):
        """Recompila com o perfil atualizado depois de desotimizar demais."""
        if method.optimized is None:
            return False
        if method.deopt_count < DEOPT_GIVEUP:
            return False
        method.optimized = None
        method.deopt_count = 0
        method._liveness = None
        method.speculation_burned = True
        rec = self.compile(method, verbose)
        if verbose:
            print("[jit] %s recompilado apos desotimizacoes; "
                  "sends residuais agora: %d"
                  % (method.full_name(), rec["residual_sends"]))
        return True


def load(path):
    with open(path) as f:
        return JIT(f.read())
