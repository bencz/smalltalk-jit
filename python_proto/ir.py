"""
IR em SSA.

O ponto central do desenho: a representacao faz parte do valor SSA, nao e
uma decisao tardia do backend. Um valor e TAGGED, F64, I64 ou BOOL, e as
conversoes entre eles sao instrucoes explicitas (box_f, unbox_f, ...).

Isso permite que a eliminacao de boxing seja simplesmente GVN sobre pares
box/unbox, em vez de uma analise especial.
"""

TAGGED = "tagged"
F64 = "f64"
I64 = "i64"
BOOL = "bool"
VOID = "void"

# ------------------------------------------------------------- opcodes
PURE = set()
TERMINATORS = {"ret", "jump", "branch"}
EFFECTFUL = {"setfieldt", "setfieldf", "vastore", "astore", "send",
             "ret", "jump",
             "branch", "guard_class", "guard_int", "vnew", "anew", "new",
             "deopt_here"}

OP_REPR = {
    "const": TAGGED, "fconst": F64, "iconst": I64, "bconst": BOOL,
    "param": TAGGED,
    "box_f": TAGGED, "box_i": TAGGED, "bool2tag": TAGGED,
    "unbox_f": F64, "unbox_i": I64, "tag2bool": BOOL,
    "i2f": F64, "f2i": I64,
    "fadd": F64, "fsub": F64, "fmul": F64, "fdiv": F64, "fsqrt": F64,
    "fneg": F64,
    "iadd": I64, "isub": I64, "imul": I64, "idiv": I64, "imod": I64,
    "fcmp": BOOL, "icmp": BOOL,
    "fieldf": F64, "fieldt": TAGGED, "setfieldt": VOID,
    "setfieldf": VOID,
    "newv": TAGGED, "new": TAGGED, "vnew": TAGGED, "anew": TAGGED,
    "vaload": F64, "vastore": VOID, "valen": I64,
    "aload": TAGGED, "astore": VOID, "alen": I64,
    "send": TAGGED, "classref": TAGGED,
    "guard_class": VOID, "guard_int": VOID,
    "ret": VOID, "jump": VOID, "branch": VOID,
    "phi": None,
}

# operacoes puras: podem ser numeradas globalmente e removidas se mortas
for _op in ("const", "fconst", "iconst", "bconst", "box_f", "box_i",
            "unbox_f", "unbox_i", "bool2tag", "tag2bool", "i2f", "f2i",
            "fadd", "fsub", "fmul", "fdiv", "fsqrt", "fneg",
            "iadd", "isub", "imul", "idiv", "imod", "fcmp", "icmp",
            "fieldf", "fieldt", "newv", "vaload", "valen", "aload",
            "alen", "classref", "param"):
    PURE.add(_op)


class Value:
    _next_id = [0]
    __slots__ = ("op", "args", "extra", "konst", "repr", "klass", "block",
                 "id", "deopt", "sel", "materialized", "loop_depth")

    def __init__(self, op, args=None, extra=None, konst=None, repr=None,
                 sel=None, deopt=None):
        self.op = op
        self.args = list(args or ())
        self.extra = extra
        self.konst = konst
        self.repr = repr if repr is not None else OP_REPR.get(op, TAGGED)
        self.klass = None          # indice de classe conhecido, ou None
        self.block = None
        self.deopt = deopt         # estado de desotimizacao (guards)
        self.sel = sel
        self.materialized = None
        self.loop_depth = 0
        self.id = Value._next_id[0]
        Value._next_id[0] += 1

    def is_pure(self):
        return self.op in PURE

    def replace_arg(self, old, new):
        for i, a in enumerate(self.args):
            if a is old:
                self.args[i] = new

    def short(self):
        return "v%d" % self.id

    def __repr__(self):
        p = ["v%d:%s = %s" % (self.id, self.repr, self.op)]
        if self.sel:
            p.append("#" + self.sel)
        if self.konst is not None:
            p.append(repr(self.konst))
        if self.extra is not None:
            p.append("{%s}" % (self.extra,))
        if self.args:
            p.append("(" + ", ".join(
                a.short() if isinstance(a, Value) else str(a)
                for a in self.args) + ")")
        if self.klass is not None:
            p.append("cls=%s" % self.klass)
        return " ".join(p)


class Block:
    _next_id = [0]

    def __init__(self, label=None):
        self.id = Block._next_id[0]
        Block._next_id[0] += 1
        self.label = label if label is not None else self.id
        self.instrs = []
        self.phis = []
        self.preds = []
        self.succs = []
        self.sealed = False
        self.filled = False
        self.defs = {}              # registrador -> Value
        self.incomplete_phis = {}   # registrador -> phi
        self.term = None
        self.loop_depth = 0

    def append(self, v):
        v.block = self
        self.instrs.append(v)
        return v

    def all_values(self):
        return self.phis + self.instrs

    def __repr__(self):
        return "B%d" % self.label


class Function:
    def __init__(self, name, method):
        self.name = name
        self.method = method
        self.blocks = []
        self.entry = None
        self.params = []
        self.deopt_frames = []

    def new_block(self, label=None):
        b = Block(label)
        self.blocks.append(b)
        return b

    def values(self):
        for b in self.blocks:
            for v in b.all_values():
                yield v
            if b.term is not None:
                yield b.term

    def compute_preds(self):
        for b in self.blocks:
            b.preds = []
        for b in self.blocks:
            for s in b.succs:
                s.preds.append(b)

    def rpo(self):
        seen, order = set(), []

        def visit(b):
            if b.id in seen:
                return
            seen.add(b.id)
            for s in b.succs:
                visit(s)
            order.append(b)
        visit(self.entry)
        order.reverse()
        return order

    def dump(self):
        out = ["function %s" % self.name]
        for b in self.rpo():
            out.append("%s:  preds=%s" % (b, [str(p) for p in b.preds]))
            for p in b.phis:
                out.append("    %s" % p)
            for v in b.instrs:
                out.append("    %s" % v)
            if b.term is not None:
                out.append("    %s  -> %s" % (b.term,
                                              [str(s) for s in b.succs]))
        return "\n".join(out)


class DeoptFrame:
    """
    Um frame virtual do interpretador reconstruivel a partir do estado
    tier 2. `slots` mapeia registrador -> Value SSA ou receita de
    materializacao.
    """

    __slots__ = ("method", "bci", "slots", "dest_reg")

    def __init__(self, method, bci, slots, dest_reg=None):
        self.method = method
        self.bci = bci
        self.slots = slots          # dict reg -> Value | Materialize
        self.dest_reg = dest_reg

    def __repr__(self):
        return "Frame(%s@%d)" % (self.method.full_name(), self.bci)


class Materialize:
    """
    Receita para recriar um objeto que a analise de escape eliminou, caso
    uma desotimizacao aconteca. E isso que torna a eliminacao segura:
    o objeto nao existe no codigo rapido, mas pode ser reconstruido no
    ponto exato em que o interpretador precisar dele.
    """

    __slots__ = ("class_index", "fields", "flat")

    def __init__(self, class_index, fields, flat):
        self.class_index = class_index
        self.fields = fields        # lista de Value
        self.flat = flat

    def __repr__(self):
        return "Materialize(cls=%d, %s)" % (
            self.class_index, [f.short() if isinstance(f, Value) else f
                               for f in self.fields])
