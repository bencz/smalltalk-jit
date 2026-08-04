"""
Da bytecode do tier 0 para IR em SSA tipada.

Duas fases:

1. `expand`: inlining no nivel do bytecode, guiado pelo perfil dos caches
   inline. Cada sitio inlinado ganha um GUARD e cada instrucao copiada
   carrega a pilha de frames virtuais que a desotimizacao vai precisar.

2. `build_ssa`: construcao de SSA pelo algoritmo de Braun et al. e, no
   mesmo passo, lowering das primitivas para operacoes tipadas com
   box/unbox explicitos. As conversoes sao inseridas de forma ingenua de
   proposito; e trabalho do otimizador remove-las.
"""

from .bc import (Instr, CONST, MOVE, SEND, GETF, SETF, JUMP, JFALSE, JTRUE,
                 RET, NEW, VNEW, ANEW, GUARD, CLASSREF)
from .ir import (Value, Block, Function, DeoptFrame, TAGGED, F64, I64, BOOL)
from .model import (Method, CI_SMALLINT, CI_FLOAT, CI_BOXEDFLOAT, CI_ARRAY,
                    CI_VALUEARRAY, CI_TRUE, CI_FALSE, CI_UNDEFINED)
from .runtime import Primitive
from .value import NIL, TRUE, FALSE, enc_int, dec_int, dec_float, is_float

MAX_INLINE_DEPTH = 4
MAX_INLINE_SIZE = 60
MIN_MONO_FRACTION = 0.90


# --------------------------------------------------------------- liveness

def _uses_defs(ins):
    op = ins.op
    if op == CONST:
        return (), (ins.d,)
    if op == MOVE:
        return (ins.a,), (ins.d,)
    if op == SEND:
        return tuple([ins.a] + list(ins.args)), (ins.d,)
    if op == GETF:
        return (ins.a,), (ins.d,)
    if op == SETF:
        return (ins.a, ins.args[0]), ()
    if op in (JFALSE, JTRUE):
        return (ins.a,), ()
    if op == JUMP:
        return (), ()
    if op == RET:
        return (ins.a,), ()
    if op in (NEW, VNEW, ANEW):
        return tuple(ins.args), (ins.d,)
    if op == CLASSREF:
        return (), (ins.d,)
    if op == GUARD:
        return (ins.a,), ()
    return (), ()


def liveness(method):
    """live_in[pc]: registradores vivos na entrada da instrucao pc."""
    cached = getattr(method, "_liveness", None)
    if cached is not None:
        return cached
    code = method.code
    n = len(code)
    succs = [[] for _ in range(n)]
    for pc, ins in enumerate(code):
        if ins.op == JUMP:
            succs[pc].append(ins.target)
        elif ins.op in (JFALSE, JTRUE):
            succs[pc].append(ins.target)
            if pc + 1 < n:
                succs[pc].append(pc + 1)
        elif ins.op == RET:
            pass
        elif pc + 1 < n:
            succs[pc].append(pc + 1)
    live_in = [frozenset() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for pc in range(n - 1, -1, -1):
            out = set()
            for s in succs[pc]:
                out |= live_in[s]
            u, d = _uses_defs(code[pc])
            new = (out - set(d)) | set(u)
            if new != live_in[pc]:
                live_in[pc] = frozenset(new)
                changed = True
    method._liveness = live_in
    return live_in


# ---------------------------------------------------------------- expansao

class InlineStats:
    def __init__(self):
        self.inlined = []
        self.rejected = []
        self.guards = 0


class Expander:
    def __init__(self, rt, root):
        self.rt = rt
        self.root = root
        self.strict = root.speculation_burned
        self.nregs = root.nregs
        self.stats = InlineStats()
        self.deps = set()          # dependencias de analise de hierarquia

    def alloc(self, n):
        b = self.nregs
        self.nregs += n
        return b

    def should_inline(self, ins, depth, chain):
        ic = ins.ic
        if ic is None or ic.megamorphic or not ic.entries:
            return None
        if self.strict and not ic.is_monomorphic():
            self.stats.rejected.append((ins.sel, "especulacao queimada"))
            return None
        ci, frac = ic.dominant()
        if ci is None or frac < MIN_MONO_FRACTION:
            return None
        callee = ic.entries.get(ci)
        if not isinstance(callee, Method):
            return None
        if depth >= MAX_INLINE_DEPTH or len(callee.code) > MAX_INLINE_SIZE:
            self.stats.rejected.append((ins.sel, "tamanho/profundidade"))
            return None
        if any(m is callee for m, _, _, _ in chain):
            self.stats.rejected.append((ins.sel, "recursao"))
            return None
        return ci, callee

    def expand(self, method, base, prefix, depth, chain):
        out = []
        mapping = {}
        patches = []          # (indice em out, pc antigo)
        ret_jumps = []
        code = method.code
        for pc, ins in enumerate(code):
            mapping[pc] = len(out)
            frames_here = prefix + ((method, pc, base, None),)
            if ins.op == SEND:
                dec = self.should_inline(ins, depth, chain)
                if dec is not None:
                    ci, callee = dec
                    g = Instr(GUARD, a=base + ins.a, extra=ci)
                    g.frames = frames_here
                    out.append(g)
                    self.stats.guards += 1
                    cbase = self.alloc(callee.nregs)
                    mv = Instr(MOVE, d=cbase, a=base + ins.a)
                    mv.frames = frames_here
                    out.append(mv)
                    for i, ar in enumerate(ins.args):
                        mv = Instr(MOVE, d=cbase + 1 + i, a=base + ar)
                        mv.frames = frames_here
                        out.append(mv)
                    child_prefix = prefix + ((method, pc, base, ins.d),)
                    sub, sub_rets = self.expand(
                        callee, cbase, child_prefix, depth + 1,
                        chain + ((method, pc, base, ins.d),))
                    off = len(out)
                    for s in sub:
                        if s.target is not None:
                            s.target += off
                        out.append(s)
                    end = len(out)
                    for idx in sub_rets:
                        out[off + idx].target = end
                    for idx in sub_rets:
                        pass
                    # o valor de retorno ja foi movido para ins.d dentro de sub
                    self.stats.inlined.append(
                        (method.full_name(), ins.sel, callee.full_name(),
                         depth))
                    continue
            new = ins.copy()
            new.frames = frames_here
            if new.d is not None:
                new.d += base
            if new.a is not None and new.op != CLASSREF:
                new.a += base
            if new.args:
                new.args = [r + base for r in new.args]
            if new.op == RET:
                if depth == 0:
                    out.append(new)
                else:
                    # dest e local ao metodo pai; precisa da base do pai
                    _pm, _ppc, parent_base, dest_local = prefix[-1]
                    mv = Instr(MOVE, d=parent_base + dest_local, a=new.a)
                    mv.frames = frames_here
                    out.append(mv)
                    j = Instr(JUMP, target=None)
                    j.frames = frames_here
                    out.append(j)
                    ret_jumps.append(len(out) - 1)
                continue
            if new.op in (JUMP, JFALSE, JTRUE):
                patches.append((len(out), ins.target))
                new.target = None
            out.append(new)
        for idx, old in patches:
            out[idx].target = mapping[old]
        return out, ret_jumps

    def run(self):
        code, _ = self.expand(self.root, 0, (), 0, ())
        for i, ins in enumerate(code):
            ins.bci = i
        return code, self.nregs


# ------------------------------------------------------- construcao de SSA

class SSABuilder:
    def __init__(self, rt, method, code, nregs):
        self.rt = rt
        self.ct = rt.ct
        self.method = method
        self.strict = method.speculation_burned
        self.code = code
        self.nregs = nregs
        self.fn = Function(method.full_name(), method)
        self.block_of = {}
        self.nil = None
        self.residual_sends = 0
        self.lowered_prims = 0
        # Classe estabelecida por um guard. Fica separada de Value.klass:
        # klass e conhecimento incondicional (resultado de uma alocacao,
        # de um box, de uma constante); isto aqui e conhecimento que so
        # vale porque um guard o garante, e portanto nao pode ser usado
        # depois para justificar a remocao daquele mesmo guard.
        self.known = {}

    # ------------------------------------------------ Braun et al. SSA
    def write_var(self, reg, block, val):
        block.defs[reg] = val

    def read_var(self, reg, block):
        v = block.defs.get(reg)
        if v is not None:
            return v
        return self.read_var_recursive(reg, block)

    def read_var_recursive(self, reg, block):
        if not block.sealed:
            phi = Value("phi", repr=TAGGED)
            phi.extra = reg
            phi.block = block
            block.phis.append(phi)
            block.incomplete_phis[reg] = phi
            val = phi
        elif len(block.preds) == 1:
            val = self.read_var(reg, block.preds[0])
        elif len(block.preds) == 0:
            val = self.nil_value()
        else:
            phi = Value("phi", repr=TAGGED)
            phi.extra = reg
            phi.block = block
            block.phis.append(phi)
            self.write_var(reg, block, phi)
            self.add_phi_operands(reg, phi)
            val = phi
        self.write_var(reg, block, val)
        return val

    def add_phi_operands(self, reg, phi):
        for p in phi.block.preds:
            phi.args.append(self.read_var(reg, p))

    def seal_block(self, block):
        if block.sealed:
            return
        block.sealed = True
        for reg, phi in list(block.incomplete_phis.items()):
            self.add_phi_operands(reg, phi)
        block.incomplete_phis.clear()

    def nil_value(self):
        if self.nil is None:
            v = Value("const", konst=NIL)
            v.klass = CI_UNDEFINED
            self.fn.entry.instrs.insert(0, v)
            v.block = self.fn.entry
            self.nil = v
        return self.nil

    # ------------------------------------------------------------ CFG
    def find_leaders(self):
        leaders = {0}
        n = len(self.code)
        for pc, ins in enumerate(self.code):
            if ins.op in (JUMP, JFALSE, JTRUE):
                leaders.add(ins.target)
                if pc + 1 < n:
                    leaders.add(pc + 1)
            elif ins.op == RET and pc + 1 < n:
                leaders.add(pc + 1)
        return sorted(leaders)

    def build_cfg(self):
        leaders = self.find_leaders()
        n = len(self.code)
        bounds = []
        for i, ld in enumerate(leaders):
            end = leaders[i + 1] if i + 1 < len(leaders) else n
            bounds.append((ld, end))
        blocks = {}
        for ld, end in bounds:
            b = self.fn.new_block(ld)
            blocks[ld] = b
        self.fn.entry = blocks[0]
        self.bounds = dict((ld, (ld, end)) for ld, end in bounds)
        self.blocks = blocks
        for ld, end in bounds:
            b = blocks[ld]
            last = self.code[end - 1]
            if last.op == JUMP:
                b.succs = [blocks[last.target]]
            elif last.op in (JFALSE, JTRUE):
                s = [blocks[last.target]]
                if end < n:
                    s.append(blocks[end])
                b.succs = s
            elif last.op == RET:
                b.succs = []
            elif end < n:
                b.succs = [blocks[end]]
            else:
                b.succs = []
        self.fn.compute_preds()
        return bounds

    # -------------------------------------------------------- construcao
    def build(self):
        bounds = self.build_cfg()
        entry = self.fn.entry
        for r in range(self.method.nregs):
            if r <= len(self.method.params):
                p = Value("param", extra=r)
                entry.append(p)
                self.fn.params.append(p)
                self.write_var(r, entry, p)
        order = self.fn.rpo()
        filled = set()
        for b in order:
            if all(p.id in filled for p in b.preds):
                self.seal_block(b)
            self.fill_block(b)
            filled.add(b.id)
            for s in b.succs:
                if all(p.id in filled for p in s.preds):
                    self.seal_block(s)
        for b in order:
            self.seal_block(b)
        return self.fn

    def fill_block(self, b):
        ld, end = self.bounds[b.label]
        self.cur = b
        for pc in range(ld, end):
            self.lower(self.code[pc], b)
        b.filled = True

    def emit(self, v):
        return self.cur.append(v)

    # --------------------------------------------------- estado de deopt
    def deopt_state(self, ins):
        frames = []
        chain = ins.frames
        for k, (meth, pc, base, dest) in enumerate(chain):
            innermost = (k == len(chain) - 1)
            live = liveness(meth)
            bci = pc if innermost else pc + 1
            lv = live[pc] if innermost else (
                live[pc + 1] if pc + 1 < len(meth.code) else frozenset())
            slots = {}
            for reg in sorted(lv):
                slots[reg] = self.read_var(base + reg, self.cur)
            if not innermost and dest is not None:
                slots.pop(dest, None)
            frames.append(DeoptFrame(meth, bci, slots,
                                     None if innermost else dest))
        return frames

    # ------------------------------------------------------- conversoes
    def to_f64(self, v):
        if v.repr == F64:
            return v
        if v.repr == I64:
            return self.emit(Value("i2f", [v]))
        if self.kls(v) == CI_SMALLINT:
            return self.emit(Value("i2f", [self.emit(Value("unbox_i", [v]))]))
        return self.emit(Value("unbox_f", [v]))

    def to_i64(self, v):
        if v.repr == I64:
            return v
        if v.repr == F64:
            return self.emit(Value("f2i", [v]))
        return self.emit(Value("unbox_i", [v]))

    def to_tagged(self, v):
        if v.repr == TAGGED:
            return v
        if v.repr == F64:
            r = self.emit(Value("box_f", [v]))
            r.klass = CI_FLOAT
            return r
        if v.repr == I64:
            r = self.emit(Value("box_i", [v]))
            r.klass = CI_SMALLINT
            return r
        r = self.emit(Value("bool2tag", [v]))
        return r

    def to_bool(self, v):
        if v.repr == BOOL:
            return v
        return self.emit(Value("tag2bool", [v]))

    def kls(self, v):
        if v.klass is not None:
            return v.klass
        return self.known.get(v.id)

    def guard(self, val, ci, ins):
        if self.kls(val) == ci:
            return None
        g = Value("guard_class", [val], extra=ci)
        g.deopt = self.deopt_state(ins)
        self.emit(g)
        self.known[val.id] = ci
        return g

    # ------------------------------------------------------- lowering
    def lower(self, ins, b):
        op = ins.op
        if op == CONST:
            k = self.rt.link_consts(ins.frames[-1][0])[ins.extra]
            v = Value("const", konst=k)
            v.klass = self.rt.class_index(k)
            self.emit(v)
            self.write_var(ins.d, b, v)
        elif op == MOVE:
            self.write_var(ins.d, b, self.read_var(ins.a, b))
        elif op == CLASSREF:
            v = Value("classref", extra=ins.extra)
            self.emit(v)
            self.write_var(ins.d, b, v)
        elif op == GUARD:
            self.guard(self.read_var(ins.a, b), ins.extra, ins)
        elif op == GETF:
            self.lower_getf(ins, b)
        elif op == SETF:
            obj = self.read_var(ins.a, b)
            val = self.read_var(ins.args[0], b)
            c = self.class_of_value(obj)
            if c is not None and c.flat:
                self.emit(Value("setfieldf", [obj, self.to_f64(val)],
                                extra=ins.b, repr="void"))
            else:
                self.emit(Value("setfieldt", [obj, self.to_tagged(val)],
                                extra=ins.b, repr="void"))
        elif op == NEW:
            self.lower_new(ins, b)
        elif op == VNEW:
            c = self.ct.get(ins.extra)
            arr_ci = self.ct.array_class_for(c).index
            v = Value("vnew", [self.to_i64(self.read_var(ins.args[0], b))],
                      extra=ins.extra)
            v.klass = arr_ci
            self.emit(v)
            self.write_var(ins.d, b, v)
        elif op == ANEW:
            v = Value("anew", [self.to_i64(self.read_var(ins.args[0], b))])
            v.klass = CI_ARRAY
            self.emit(v)
            self.write_var(ins.d, b, v)
        elif op == SEND:
            self.lower_send(ins, b)
        elif op == RET:
            t = Value("ret", [self.to_tagged(self.read_var(ins.a, b))],
                      repr="void")
            b.term = t
            t.block = b
        elif op == JUMP:
            t = Value("jump", repr="void")
            b.term = t
            t.block = b
        elif op in (JFALSE, JTRUE):
            c = self.to_bool(self.read_var(ins.a, b))
            t = Value("branch", [c], extra=(op == JTRUE), repr="void")
            b.term = t
            t.block = b
        else:
            raise NotImplementedError(op)

    def class_of_value(self, v):
        k = self.kls(v)
        if k is None:
            return None
        return self.ct.get(k)

    def lower_getf(self, ins, b):
        obj = self.read_var(ins.a, b)
        c = self.class_of_value(obj)
        if c is not None and c.flat:
            f = self.emit(Value("fieldf", [obj], extra=ins.b))
            self.write_var(ins.d, b, self.to_tagged(f))
        else:
            v = self.emit(Value("fieldt", [obj], extra=ins.b))
            if c is not None and ins.b < len(c.fields):
                ft = c.ftypes.get(c.fields[ins.b])
                if ft is not None and ft not in ("Float64", "Int64"):
                    tc = self.ct.find(ft)
                    if tc is not None:
                        v.klass = tc.index
            self.write_var(ins.d, b, v)

    def lower_new(self, ins, b):
        c = self.ct.get(ins.extra)
        args = [self.read_var(r, b) for r in ins.args]
        if c.flat:
            fargs = [self.to_f64(a) for a in args]
            while len(fargs) < len(c.fields):
                fargs.append(self.emit(Value("fconst", konst=0.0)))
            v = Value("newv", fargs, extra=c.index)
        else:
            v = Value("new", [self.to_tagged(a) for a in args],
                      extra=c.index)
        v.klass = c.index
        self.emit(v)
        self.write_var(ins.d, b, v)

    # ------------------------------------------------------------ sends
    def lower_send(self, ins, b):
        recv = self.read_var(ins.a, b)
        args = [self.read_var(r, b) for r in ins.args]
        ic = ins.ic
        ci, frac = (ic.dominant() if ic is not None else (None, 0.0))
        if self.strict and (ic is None or not ic.is_monomorphic()):
            ci = None
        if ci is not None and frac >= MIN_MONO_FRACTION:
            callee = ic.entries.get(ci)
            if isinstance(callee, Primitive):
                res = self.lower_primitive(ins, b, ci, recv, args)
                if res is not None:
                    self.lowered_prims += 1
                    self.write_var(ins.d, b, res)
                    return
        self.residual_sends += 1
        v = Value("send", [self.to_tagged(recv)] +
                  [self.to_tagged(a) for a in args], sel=ins.sel)
        v.deopt = self.deopt_state(ins)
        self.emit(v)
        self.write_var(ins.d, b, v)

    def arg_class(self, ic):
        if ic is None or not ic.arg_counts:
            return None
        return max(ic.arg_counts, key=ic.arg_counts.get)

    FLOATCLS = (CI_FLOAT, CI_BOXEDFLOAT)
    FOPS = {"+": "fadd", "-": "fsub", "*": "fmul", "/": "fdiv"}
    IOPS = {"+": "iadd", "-": "isub", "*": "imul", "//": "idiv",
            "\\\\": "imod"}
    CMPS = ("<", ">", "<=", ">=", "=", "~=")

    def lower_primitive(self, ins, b, ci, recv, args):
        sel = ins.sel
        aci = self.arg_class(ins.ic)
        ct = self.ct
        c = ct.get(ci)

        # ---- aritmetica
        if sel in self.FOPS or sel in self.IOPS or sel in self.CMPS:
            if aci is None:
                return None
            both_int = (ci == CI_SMALLINT and aci == CI_SMALLINT)
            numeric = (ci in (CI_SMALLINT,) + self.FLOATCLS
                       and aci in (CI_SMALLINT,) + self.FLOATCLS)
            if not numeric:
                return None
            self.guard(recv, ci, ins)
            self.guard(args[0], aci, ins)
            if both_int:
                if sel in self.IOPS:
                    r = self.emit(Value(self.IOPS[sel],
                                        [self.to_i64(recv),
                                         self.to_i64(args[0])]))
                    return self.to_tagged(r)
                if sel in self.CMPS:
                    r = self.emit(Value("icmp",
                                        [self.to_i64(recv),
                                         self.to_i64(args[0])], extra=sel))
                    return r
                if sel == "/":
                    pass
            x, y = self.to_f64(recv), self.to_f64(args[0])
            if sel in self.FOPS:
                return self.to_tagged(self.emit(Value(self.FOPS[sel], [x, y])))
            if sel in self.CMPS:
                return self.emit(Value("fcmp", [x, y], extra=sel))
            return None

        if sel == "asFloat" and ci in (CI_SMALLINT,) + self.FLOATCLS:
            self.guard(recv, ci, ins)
            return self.to_tagged(self.to_f64(recv))

        if sel == "sqrt" and ci in (CI_SMALLINT,) + self.FLOATCLS:
            self.guard(recv, ci, ins)
            return self.to_tagged(
                self.emit(Value("fsqrt", [self.to_f64(recv)])))

        # ---- arrays de valor com layout plano
        if c.array_of is not None:
            elem = c.array_of
            self.guard(recv, ci, ins)
            if sel == "size":
                return self.to_tagged(self.emit(Value("valen", [recv])))
            if sel == "at:" and aci == CI_SMALLINT:
                self.guard(args[0], CI_SMALLINT, ins)
                idx = self.to_i64(args[0])
                fields = [self.emit(Value("vaload", [recv, idx], extra=k))
                          for k in range(len(elem.fields))]
                v = self.emit(Value("newv", fields, extra=elem.index))
                v.klass = elem.index
                return v
            if sel == "at:put:" and aci == CI_SMALLINT:
                self.guard(args[0], CI_SMALLINT, ins)
                self.guard(args[1], elem.index, ins)
                idx = self.to_i64(args[0])
                src = args[1]
                fs = [self.emit(Value("fieldf", [src], extra=k))
                      for k in range(len(elem.fields))]
                self.emit(Value("vastore", [recv, idx] + fs, repr="void"))
                return src
            return None

        # ---- Array generico
        if ci == CI_ARRAY:
            self.guard(recv, ci, ins)
            if sel == "size":
                return self.to_tagged(self.emit(Value("alen", [recv])))
            if sel == "at:" and aci == CI_SMALLINT:
                self.guard(args[0], CI_SMALLINT, ins)
                return self.emit(Value("aload",
                                       [recv, self.to_i64(args[0])]))
            if sel == "at:put:" and aci == CI_SMALLINT:
                self.guard(args[0], CI_SMALLINT, ins)
                self.emit(Value("astore",
                                [recv, self.to_i64(args[0]),
                                 self.to_tagged(args[1])], repr="void"))
                return args[1]
            return None
        return None


def build_function(rt, method):
    ex = Expander(rt, method)
    code, nregs = ex.run()
    sb = SSABuilder(rt, method, code, nregs)
    fn = sb.build()
    fn.inline_stats = ex.stats
    fn.residual_sends = sb.residual_sends
    fn.lowered_prims = sb.lowered_prims
    return fn
