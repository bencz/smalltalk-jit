"""
Tier 2: da SSA otimizada para codigo executavel.

Nao ha geracao de codigo de maquina aqui. Cada instrucao vira uma closure
Python tipada, operando sobre um arquivo de registradores plano. O que
importa para a validacao e que os registradores F64 guardam doubles crus,
os I64 guardam inteiros crus, e nenhuma operacao do laco quente toca os
contadores de tagging ou de alocacao do value.py.

A alocacao de registradores e trivial (um slot por valor SSA, sem reuso).
Um sistema real usaria linear scan com splitting; isso nao muda nada do
que este prototipo se propoe a demonstrar.
"""

from .ir import Value, Materialize, TAGGED, F64, I64, BOOL, VOID
from .value import (STATS, NIL, TRUE, FALSE, enc_int, dec_int, enc_float,
                    dec_float, is_int, is_float)
from .runtime import DeoptRequest
from .model import CI_VALUEARRAY


class Tier2Code:
    def __init__(self, fn, method):
        self.fn = fn
        self.method = method
        self.nslots = 0
        self.blocks = []
        self.edge_copies = {}
        self.param_slots = []
        self.block_index = {}
        self.n_guards = 0


class BlockCode:
    __slots__ = ("ops", "term")

    def __init__(self):
        self.ops = []
        self.term = None


class Compiler:
    def __init__(self, rt, fn, method):
        self.rt = rt
        self.fn = fn
        self.method = method
        self.slot = {}
        self.code = Tier2Code(fn, method)

    def s(self, v):
        k = self.slot.get(v.id)
        if k is None:
            k = self.code.nslots
            self.code.nslots += 1
            self.slot[v.id] = k
        return k

    def compile(self):
        order = self.fn.rpo()
        for i, b in enumerate(order):
            self.code.block_index[b.id] = i
        for b in order:
            for v in b.phis:
                self.s(v)
            for v in b.instrs:
                self.s(v)
        for b in order:
            bc = BlockCode()
            for v in b.instrs:
                f = self.emit(v)
                if f is not None:
                    bc.ops.append(f)
            bc.term = self.emit_term(b)
            self.code.blocks.append(bc)
        # copias de phi por aresta
        for b in order:
            if not b.phis:
                continue
            for i, p in enumerate(b.preds):
                cps = []
                for phi in b.phis:
                    src = phi.args[i]
                    cps.append((self.s(phi), self.s(src)))
                self.code.edge_copies[
                    (self.code.block_index[p.id],
                     self.code.block_index[b.id])] = cps
        for v in self.fn.params:
            self.code.param_slots.append((v.extra, self.s(v)))
        return self.code

    # ------------------------------------------------------------ deopt
    def build_deopt(self, frames):
        rt = self.rt
        plans = []
        for fr in frames:
            slots = []
            for reg, val in sorted(fr.slots.items()):
                if isinstance(val, Materialize):
                    fslots = [(self.s(f), f.repr) for f in val.fields]
                    slots.append((reg, "mat", (val.class_index, val.flat,
                                               fslots)))
                else:
                    slots.append((reg, "val", (self.s(val), val.repr)))
            plans.append((fr.method, fr.bci, fr.method.nregs, slots,
                          fr.dest_reg))

        def materialize(R):
            out = []
            for meth, bci, nregs, slots, dest in plans:
                regs = [NIL] * nregs
                for reg, kind, payload in slots:
                    if kind == "val":
                        sl, rp = payload
                        regs[reg] = self.retag(R[sl], rp)
                    else:
                        ci, flat, fslots = payload
                        vals = [R[sl] if rp == F64 else
                                self.untag_f(R[sl], rp)
                                for sl, rp in fslots]
                        if flat:
                            regs[reg] = rt.heap.alloc(ci, list(vals))
                        else:
                            regs[reg] = rt.heap.alloc(
                                ci, [self.retag(v, TAGGED) for v in vals])
                out.append((meth, bci, regs, dest))
            return out
        return materialize

    def retag(self, x, rp):
        if rp == F64:
            return self.rt.make_float(x)
        if rp == I64:
            return self.rt.make_int(x)
        if rp == BOOL:
            return TRUE if x else FALSE
        return x

    def untag_f(self, x, rp):
        if rp == F64:
            return x
        if rp == I64:
            return float(x)
        return self.rt.as_number(x)

    # ---------------------------------------------------------- emissao
    def emit_term(self, b):
        bi = self.code.block_index
        t = b.term
        if t is None:
            if b.succs:
                nxt = bi[b.succs[0].id]
                return lambda R: (1, nxt)
            return lambda R: (0, NIL)
        if t.op == "ret":
            a = self.s(t.args[0])
            return lambda R: (0, R[a])
        if t.op == "jump":
            nxt = bi[b.succs[0].id]
            return lambda R: (1, nxt)
        if t.op == "branch":
            c = self.s(t.args[0])
            on_true = t.extra
            tgt = bi[b.succs[0].id]
            fall = bi[b.succs[1].id] if len(b.succs) > 1 else tgt
            if on_true:
                return lambda R: (1, tgt if R[c] else fall)
            return lambda R: (1, tgt if not R[c] else fall)
        raise NotImplementedError(t.op)

    def emit(self, v):
        rt = self.rt
        heap = rt.heap
        op = v.op
        d = self.s(v)
        A = [self.s(a) for a in v.args if isinstance(a, Value)]

        if op == "const":
            k = v.konst
            def f(R, d=d, k=k): R[d] = k
            return f
        if op == "fconst":
            k = float(v.konst)
            def f(R, d=d, k=k): R[d] = k
            return f
        if op == "iconst":
            k = int(v.konst)
            def f(R, d=d, k=k): R[d] = k
            return f
        if op == "param":
            return None
        if op == "classref":
            ci = v.extra
            def f(R, d=d, ci=ci): R[d] = rt.class_object(ci)
            return f

        if op == "box_f":
            a = A[0]
            def f(R, d=d, a=a): R[d] = rt.make_float(R[a])
            return f
        if op == "box_i":
            a = A[0]
            def f(R, d=d, a=a): R[d] = rt.make_int(R[a])
            return f
        if op == "bool2tag":
            a = A[0]
            def f(R, d=d, a=a): R[d] = TRUE if R[a] else FALSE
            return f
        if op == "unbox_f":
            a = A[0]
            def f(R, d=d, a=a): R[d] = rt.as_number(R[a])
            return f
        if op == "unbox_i":
            a = A[0]
            def f(R, d=d, a=a): R[d] = dec_int(R[a])
            return f
        if op == "tag2bool":
            a = A[0]
            def f(R, d=d, a=a): R[d] = (R[a] == TRUE)
            return f
        if op == "i2f":
            a = A[0]
            def f(R, d=d, a=a): R[d] = float(R[a])
            return f
        if op == "f2i":
            a = A[0]
            def f(R, d=d, a=a): R[d] = int(R[a])
            return f

        # ---- aritmetica crua: nenhum contador de tagging e tocado
        if op in ("fadd", "fsub", "fmul", "fdiv"):
            a, b = A
            if op == "fadd":
                def f(R, d=d, a=a, b=b): R[d] = R[a] + R[b]
            elif op == "fsub":
                def f(R, d=d, a=a, b=b): R[d] = R[a] - R[b]
            elif op == "fmul":
                def f(R, d=d, a=a, b=b): R[d] = R[a] * R[b]
            else:
                def f(R, d=d, a=a, b=b): R[d] = R[a] / R[b]
            return f
        if op == "fsqrt":
            a = A[0]
            def f(R, d=d, a=a): R[d] = R[a] ** 0.5
            return f
        if op == "fneg":
            a = A[0]
            def f(R, d=d, a=a): R[d] = -R[a]
            return f
        if op in ("iadd", "isub", "imul", "idiv", "imod"):
            a, b = A
            if op == "iadd":
                def f(R, d=d, a=a, b=b): R[d] = R[a] + R[b]
            elif op == "isub":
                def f(R, d=d, a=a, b=b): R[d] = R[a] - R[b]
            elif op == "imul":
                def f(R, d=d, a=a, b=b): R[d] = R[a] * R[b]
            elif op == "idiv":
                def f(R, d=d, a=a, b=b): R[d] = R[a] // R[b]
            else:
                def f(R, d=d, a=a, b=b): R[d] = R[a] % R[b]
            return f
        if op in ("fcmp", "icmp"):
            a, b = A
            k = v.extra
            if k == "<":
                def f(R, d=d, a=a, b=b): R[d] = R[a] < R[b]
            elif k == ">":
                def f(R, d=d, a=a, b=b): R[d] = R[a] > R[b]
            elif k == "<=":
                def f(R, d=d, a=a, b=b): R[d] = R[a] <= R[b]
            elif k == ">=":
                def f(R, d=d, a=a, b=b): R[d] = R[a] >= R[b]
            elif k == "=":
                def f(R, d=d, a=a, b=b): R[d] = R[a] == R[b]
            else:
                def f(R, d=d, a=a, b=b): R[d] = R[a] != R[b]
            return f

        # ---- memoria
        if op == "fieldf":
            a, k = A[0], v.extra
            def f(R, d=d, a=a, k=k):
                R[d] = heap.deref_raw(R[a]).fields[k]
            return f
        if op == "fieldt":
            a, k = A[0], v.extra
            def f(R, d=d, a=a, k=k):
                STATS.field_loads += 1
                R[d] = heap.deref(R[a]).fields[k]
            return f
        if op == "setfieldf":
            a, b, k = A[0], A[1], v.extra
            def f(R, a=a, b=b, k=k):
                heap.deref_raw(R[a]).fields[k] = R[b]
            return f
        if op == "setfieldt":
            a, b, k = A[0], A[1], v.extra
            def f(R, a=a, b=b, k=k):
                heap.deref(R[a]).fields[k] = R[b]
            return f
        if op == "vaload":
            a, i, k = A[0], A[1], v.extra
            def f(R, d=d, a=a, i=i, k=k):
                va = heap.objects[R[a] >> 3]
                R[d] = va.data[R[i] * va.nfields + k]
            return f
        if op == "vastore":
            a, i = A[0], A[1]
            fs = A[2:]
            def f(R, a=a, i=i, fs=fs):
                va = heap.objects[R[a] >> 3]
                base = R[i] * va.nfields
                for j, sl in enumerate(fs):
                    va.data[base + j] = R[sl]
            return f
        if op == "valen":
            a = A[0]
            def f(R, d=d, a=a): R[d] = heap.objects[R[a] >> 3].count
            return f
        if op == "aload":
            a, i = A
            def f(R, d=d, a=a, i=i):
                STATS.tagged_loads += 1
                R[d] = heap.deref(R[a]).data[R[i]]
            return f
        if op == "astore":
            a, i, x = A
            def f(R, a=a, i=i, x=x):
                heap.deref(R[a]).data[R[i]] = R[x]
            return f
        if op == "alen":
            a = A[0]
            def f(R, d=d, a=a): R[d] = len(heap.deref(R[a]).data)
            return f

        # ---- alocacao
        if op == "newv":
            ci = v.extra
            fs = A
            def f(R, d=d, ci=ci, fs=fs):
                R[d] = heap.alloc(ci, [R[s] for s in fs])
            return f
        if op == "new":
            ci = v.extra
            fs = A
            c = rt.ct.get(ci)
            n = len(c.fields)
            def f(R, d=d, ci=ci, fs=fs, n=n):
                vals = [R[s] for s in fs]
                vals += [NIL] * (n - len(vals))
                R[d] = heap.alloc(ci, vals)
            return f
        if op == "vnew":
            ci = v.extra
            c = rt.ct.get(ci)
            ac = rt.ct.array_class_for(c)
            nf = len(c.fields)
            a = A[0]
            def f(R, d=d, a=a, aci=ac.index, ci=ci, nf=nf):
                R[d] = heap.alloc_value_array(aci, ci, nf, R[a])
            return f
        if op == "anew":
            a = A[0]
            def f(R, d=d, a=a):
                R[d] = heap.alloc_generic_array(7, R[a])
            return f

        # ---- guards e sends residuais
        if op == "guard_class":
            a = A[0]
            ci = v.extra
            self.code.n_guards += 1
            mat = self.build_deopt(v.deopt or [])
            def f(R, a=a, ci=ci, mat=mat):
                STATS.guard_checks += 1
                if rt.class_index(R[a]) != ci:
                    STATS.guard_fails += 1
                    raise DeoptRequest(mat(R))
            return f
        if op == "send":
            sel = v.sel
            r = A[0]
            argsl = A[1:]
            def f(R, d=d, sel=sel, r=r, argsl=argsl):
                R[d] = rt.send(sel, R[r], tuple(R[s] for s in argsl))
            return f

        raise NotImplementedError("backend: %s" % op)


def compile_tier2(rt, fn, method):
    return Compiler(rt, fn, method).compile()


def run_tier2(rt, code, recv, args):
    R = [NIL] * code.nslots
    for reg, sl in code.param_slots:
        if reg == 0:
            R[sl] = recv
        elif args is not None and reg - 1 < len(args):
            R[sl] = args[reg - 1]
    blocks = code.blocks
    edges = code.edge_copies
    cur, prev = 0, -1
    while True:
        if prev >= 0:
            cps = edges.get((prev, cur))
            if cps:
                tmp = [R[s] for _, s in cps]
                for (dst, _), t in zip(cps, tmp):
                    R[dst] = t
        blk = blocks[cur]
        for f in blk.ops:
            f(R)
        kind, x = blk.term(R)
        if kind == 0:
            return x
        prev, cur = cur, x
