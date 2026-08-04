"""
Bytecode de registradores, tres enderecos.

A escolha por registradores em vez de pilha custa um pouco mais de espaco
mas reduz o despacho pela metade e, principalmente, da ao tier 1 um
mapeamento quase direto para registradores fisicos e ao tier 2 uma
construcao de SSA trivial.

Cada SEND carrega um ICSite, que e simultaneamente o cache inline
(aceleracao) e o coletor de perfil de tipos (informacao para o otimizador).
"""

CONST = "CONST"
MOVE = "MOVE"
SEND = "SEND"
GETF = "GETF"
SETF = "SETF"
JUMP = "JUMP"
JFALSE = "JFALSE"
JTRUE = "JTRUE"
RET = "RET"
NEW = "NEW"
VNEW = "VNEW"
ANEW = "ANEW"
GUARD = "GUARD"
CLASSREF = "CLASSREF"


class ICSite:
    """
    Cache inline polimorfico.

    Ate MAX_ENTRIES pares (class_index -> metodo). Alem disso o sitio vira
    megamorfico e deixa de ser util como fonte de perfil, o que e
    exatamente a informacao que o otimizador precisa para nao inlinar.
    """

    MAX_ENTRIES = 6

    __slots__ = ("sel", "entries", "counts", "megamorphic", "total",
                 "arg_counts")

    def __init__(self, sel):
        self.sel = sel
        self.entries = {}      # class_index -> callee (Method ou primitivo)
        self.counts = {}       # class_index -> contagem
        self.arg_counts = {}   # class_index do primeiro argumento
        self.megamorphic = False
        self.total = 0

    def record_arg(self, ci):
        self.arg_counts[ci] = self.arg_counts.get(ci, 0) + 1

    def record(self, ci, callee):
        self.total += 1
        if ci in self.counts:
            self.counts[ci] += 1
            return
        if len(self.entries) >= self.MAX_ENTRIES:
            self.megamorphic = True
            return
        self.entries[ci] = callee
        self.counts[ci] = 1

    def is_monomorphic(self):
        return not self.megamorphic and len(self.entries) == 1

    def dominant(self):
        """Classe majoritaria e sua fracao, ou (None, 0.0)."""
        if self.megamorphic or not self.counts:
            return None, 0.0
        ci = max(self.counts, key=self.counts.get)
        return ci, self.counts[ci] / float(self.total)

    def sorted_entries(self):
        return sorted(self.counts.items(), key=lambda kv: -kv[1])


class Instr:
    __slots__ = ("op", "d", "a", "b", "args", "target", "sel", "ic",
                 "extra", "bci", "frames", "quick")

    def __init__(self, op, d=None, a=None, b=None, args=None, target=None,
                 sel=None, ic=None, extra=None, frames=None):
        self.op = op
        self.d = d
        self.a = a
        self.b = b
        self.args = args or []
        self.target = target
        self.sel = sel
        self.ic = ic
        self.extra = extra
        self.bci = -1
        # Pilha virtual de frames para desotimizacao, do mais externo ao
        # mais interno: [(method, bci, reg_base), ...]
        self.frames = frames
        self.quick = None

    def copy(self):
        i = Instr(self.op, self.d, self.a, self.b, list(self.args),
                  self.target, self.sel, self.ic, self.extra, self.frames)
        return i

    def __repr__(self):
        p = [self.op]
        if self.d is not None:
            p.append("r%d <-" % self.d)
        if self.sel:
            p.append("#" + self.sel)
        if self.a is not None:
            p.append("r%d" % self.a)
        if self.b is not None:
            p.append("%s" % self.b)
        if self.args:
            p.append("(" + ", ".join("r%d" % r for r in self.args) + ")")
        if self.target is not None:
            p.append("-> L%s" % self.target)
        if self.extra is not None:
            p.append("{%s}" % (self.extra,))
        return " ".join(p)


def disasm(code, indent="    "):
    out = []
    for i, ins in enumerate(code):
        out.append("%s%3d: %s" % (indent, i, ins))
    return "\n".join(out)
