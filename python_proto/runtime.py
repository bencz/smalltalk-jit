"""
Tier 0: interpretador de bytecode com caches inline.

O interpretador e deliberadamente simples. O que importa aqui nao e a sua
velocidade, e sim que todo SEND passe por um ICSite, porque e dali que o
otimizador vai tirar o perfil de tipos.

Layout de objeto:
  - classe Object: campos sao palavras tagged.
  - classe Value com campos Float64: campos sao doubles crus, embutidos.
    Ler um desses campos para um contexto generico exige codificar
    (contado como tagged_op, e como box quando nao cabe no imediato);
    construir um desses objetos exige decodificar os argumentos
    (contado como unbox). E exatamente esse trafego que o tier 2 elimina.
"""

from .value import (
    STATS, NIL, TRUE, FALSE, enc_int, dec_int, is_int, is_float, is_ptr,
    enc_float, dec_float, float_fits, Heap, Obj, ValueArray, GenericArray,
    BoxedFloat,
)
from .bc import (CONST, MOVE, SEND, GETF, SETF, JUMP, JFALSE, JTRUE, RET,
                 NEW, VNEW, ANEW, GUARD, CLASSREF)
from .model import (
    CI_UNDEFINED, CI_FALSE, CI_TRUE, CI_SMALLINT, CI_FLOAT, CI_BOXEDFLOAT,
    CI_ARRAY, CI_VALUEARRAY, CI_STRING, CI_SYMBOL, CI_CLASS, Method,
)


class STError(Exception):
    pass


class DeoptRequest(Exception):
    """Levantada pelo codigo tier 2 quando um guard especulativo falha."""

    def __init__(self, frames):
        Exception.__init__(self, "deopt")
        self.frames = frames


class Primitive:
    __slots__ = ("name", "fn", "nargs")

    def __init__(self, name, fn, nargs):
        self.name = name
        self.fn = fn
        self.nargs = nargs

    def __repr__(self):
        return "<prim %s>" % self.name


class Runtime:
    def __init__(self, ct):
        self.ct = ct
        self.heap = Heap(CI_BOXEDFLOAT)
        self.prims = {}
        self.class_objects = {}
        self.string_pool = {}
        self.tier2_enabled = True
        self.trace = False
        self._install_primitives()
        self._prepare_classes()

    # -------------------------------------------------------- preparacao
    def _prepare_classes(self):
        for c in self.ct.classes:
            c.field_is_f64 = [c.ftypes.get(f) == "Float64" for f in c.fields]
            c.flat = c.is_value() and c.all_float_fields()

    def class_object(self, ci):
        w = self.class_objects.get(ci)
        if w is None:
            w = self.heap.alloc(CI_CLASS, [enc_int(ci)])
            self.class_objects[ci] = w
        return w

    def link_consts(self, m):
        if getattr(m, "kconsts", None) is not None:
            return m.kconsts
        out = []
        for kind, val in m.consts:
            if kind == "int":
                out.append(enc_int(val))
            elif kind == "float":
                out.append(enc_float(val, self.heap))
            elif kind == "nil":
                out.append(NIL)
            elif kind == "bool":
                out.append(TRUE if val else FALSE)
            elif kind in ("str", "sym"):
                key = (kind, val)
                w = self.string_pool.get(key)
                if w is None:
                    ci = CI_STRING if kind == "str" else CI_SYMBOL
                    w = self.heap.alloc(ci, [val])
                    self.string_pool[key] = w
                out.append(w)
            else:
                raise STError("constante desconhecida %r" % (kind,))
        m.kconsts = out
        return out

    # ------------------------------------------------------------- tipos
    def class_index(self, w):
        if (w & 1) == 0:
            return CI_SMALLINT
        t = w & 0b111
        if t == 0b111:
            return CI_FLOAT
        if t == 0b101:
            if w == NIL:
                return CI_UNDEFINED
            if w == TRUE:
                return CI_TRUE
            if w == FALSE:
                return CI_FALSE
            return CI_UNDEFINED
        if t == 0b001:
            return self.heap.deref_raw(w).class_index
        return CI_UNDEFINED

    def class_name(self, w):
        return self.ct.get(self.class_index(w)).name

    # ------------------------------------------- conversao numerica
    def as_number(self, w):
        if (w & 1) == 0:
            return dec_int(w)
        if (w & 0b111) == 0b111:
            STATS.unboxes += 1
            return dec_float(w)
        if (w & 0b111) == 0b001:
            o = self.heap.deref_raw(w)
            if isinstance(o, BoxedFloat):
                STATS.unboxes += 1
                return o.value
        raise STError("nao e um numero: %s" % self.class_name(w))

    def is_float_val(self, w):
        if (w & 0b111) == 0b111:
            return True
        if (w & 0b111) == 0b001:
            return isinstance(self.heap.deref_raw(w), BoxedFloat)
        return False

    def make_float(self, d):
        STATS.tagged_ops += 1
        return enc_float(d, self.heap)

    def make_int(self, v):
        STATS.tagged_ops += 1
        return enc_int(v)

    # ---------------------------------------------------------- objetos
    def instantiate(self, ci, argwords):
        c = self.ct.get(ci)
        if c.flat:
            vals = []
            for w in argwords:
                vals.append(self.as_number(w))
            while len(vals) < len(c.fields):
                vals.append(0.0)
            return self.heap.alloc(ci, vals)
        fields = list(argwords) + [NIL] * (len(c.fields) - len(argwords))
        return self.heap.alloc(ci, fields)

    def get_field(self, w, idx):
        o = self.heap.deref(w)
        c = self.ct.get(o.class_index)
        v = o.fields[idx]
        if c.flat:
            return self.make_float(v)
        STATS.field_loads += 1
        return v

    def set_field(self, w, idx, val):
        o = self.heap.deref(w)
        c = self.ct.get(o.class_index)
        if c.flat:
            o.fields[idx] = self.as_number(val)
        else:
            o.fields[idx] = val

    # ------------------------------------------------------- primitivas
    def _install_primitives(self):
        rt = self

        def arith(name, fn, intfn=None):
            def p(recv, args):
                b = args[0]
                if intfn is not None and (recv & 1) == 0 and (b & 1) == 0:
                    STATS.i64_ops += 1
                    return rt.make_int(intfn(dec_int(recv), dec_int(b)))
                STATS.f64_ops += 1
                return rt.make_float(fn(rt.as_number(recv),
                                        rt.as_number(b)))
            return Primitive(name, p, 1)

        def cmp_(name, fn):
            def p(recv, args):
                a = rt.as_number(recv)
                b = rt.as_number(args[0])
                STATS.tagged_ops += 1
                return TRUE if fn(a, b) else FALSE
            return Primitive(name, p, 1)

        numeric = {
            "+": arith("+", lambda a, b: a + b, lambda a, b: a + b),
            "-": arith("-", lambda a, b: a - b, lambda a, b: a - b),
            "*": arith("*", lambda a, b: a * b, lambda a, b: a * b),
            "/": arith("/", lambda a, b: a / b),
            "//": arith("//", lambda a, b: a // b, lambda a, b: a // b),
            "\\\\": arith("\\\\", lambda a, b: a % b, lambda a, b: a % b),
            "<": cmp_("<", lambda a, b: a < b),
            ">": cmp_(">", lambda a, b: a > b),
            "<=": cmp_("<=", lambda a, b: a <= b),
            ">=": cmp_(">=", lambda a, b: a >= b),
            "=": cmp_("=", lambda a, b: a == b),
            "~=": cmp_("~=", lambda a, b: a != b),
        }
        for ci in (CI_SMALLINT, CI_FLOAT, CI_BOXEDFLOAT):
            for sel, p in numeric.items():
                self.prims[(ci, sel)] = p

        def p_asfloat(recv, args):
            return rt.make_float(float(rt.as_number(recv)))
        for ci in (CI_SMALLINT, CI_FLOAT, CI_BOXEDFLOAT):
            self.prims[(ci, "asFloat")] = Primitive("asFloat", p_asfloat, 0)

        def p_sqrt(recv, args):
            STATS.f64_ops += 1
            return rt.make_float(rt.as_number(recv) ** 0.5)
        for ci in (CI_SMALLINT, CI_FLOAT, CI_BOXEDFLOAT):
            self.prims[(ci, "sqrt")] = Primitive("sqrt", p_sqrt, 0)

        def p_truncated(recv, args):
            return rt.make_int(int(rt.as_number(recv)))
        for ci in (CI_FLOAT, CI_BOXEDFLOAT, CI_SMALLINT):
            self.prims[(ci, "truncated")] = Primitive("truncated",
                                                      p_truncated, 0)

        # ---- Array generico
        def p_arr_at(recv, args):
            o = rt.heap.deref(recv)
            STATS.tagged_loads += 1
            return o.data[dec_int(args[0])]
        self.prims[(CI_ARRAY, "at:")] = Primitive("Array>>at:", p_arr_at, 1)

        def p_arr_atput(recv, args):
            o = rt.heap.deref(recv)
            o.data[dec_int(args[0])] = args[1]
            return args[1]
        self.prims[(CI_ARRAY, "at:put:")] = Primitive("Array>>at:put:",
                                                      p_arr_atput, 2)

        def p_arr_size(recv, args):
            return rt.make_int(len(rt.heap.deref(recv).data))
        self.prims[(CI_ARRAY, "size")] = Primitive("Array>>size",
                                                   p_arr_size, 0)

        # ---- ValueArray com layout plano
        def p_va_at(recv, args):
            va = rt.heap.deref(recv)
            i = dec_int(args[0])
            k = va.nfields
            base = i * k
            # materializa um objeto para devolver ao contexto generico
            return rt.heap.alloc(va.elem_class_index,
                                 va.data[base:base + k])
        self.prims[(CI_VALUEARRAY, "at:")] = Primitive("ValueArray>>at:",
                                                       p_va_at, 1)

        def p_va_atput(recv, args):
            va = rt.heap.deref(recv)
            i = dec_int(args[0])
            k = va.nfields
            src = rt.heap.deref(args[1])
            va.data[i * k:i * k + k] = src.fields
            return args[1]
        self.prims[(CI_VALUEARRAY, "at:put:")] = Primitive(
            "ValueArray>>at:put:", p_va_atput, 2)

        def p_va_size(recv, args):
            return rt.make_int(rt.heap.deref(recv).count)
        self.prims[(CI_VALUEARRAY, "size")] = Primitive("ValueArray>>size",
                                                        p_va_size, 0)

        def p_identity(recv, args):
            return recv
        for ci in range(len(getattr(self.ct, "classes", []))):
            pass
        self.prim_identity = Primitive("yourself", p_identity, 0)

    # ---------------------------------------------------------- lookup
    def lookup(self, ci, sel):
        c = self.ct.get(ci)
        m = c.methods.get(sel)
        if m is not None:
            return m
        p = self.prims.get((ci, sel))
        if p is not None:
            return p
        if c.array_of is not None:
            p = self.prims.get((CI_VALUEARRAY, sel))
            if p is not None:
                return p
        if sel == "yourself":
            return self.prim_identity
        if sel == "class":
            return None
        return None

    # ----------------------------------------------------------- execucao
    def send(self, sel, recv, args, ic=None):
        ci = self.class_index(recv)
        STATS.sends += 1
        if ic is not None and args:
            ic.record_arg(self.class_index(args[0]))
        callee = None
        if ic is not None:
            callee = ic.entries.get(ci)
            if callee is not None:
                STATS.ic_hits += 1
        if callee is None:
            STATS.ic_misses += 1
            callee = self.lookup(ci, sel)
            if callee is None:
                raise STError("doesNotUnderstand: %s enviado a %s"
                              % (sel, self.ct.get(ci).name))
            if ic is not None:
                ic.record(ci, callee)
        else:
            ic.counts[ci] = ic.counts[ci] + 1
            ic.total += 1
        return self.invoke(callee, recv, args)

    def invoke(self, callee, recv, args):
        if isinstance(callee, Primitive):
            return callee.fn(recv, args)
        return self.execute(callee, recv, args)

    def execute(self, m, recv, args):
        m.calls += 1
        if m.optimized is not None and self.tier2_enabled:
            return self.run_optimized(m, recv, args)
        return self.interpret(m, recv, args)

    def run_optimized(self, m, recv, args):
        from .backend import run_tier2
        try:
            return run_tier2(self, m.optimized, recv, args)
        except DeoptRequest as d:
            STATS.deopts += 1
            m.deopt_count += 1
            return self.resume_deopt(d.frames)

    def resume_deopt(self, frames):
        """
        Reconstroi os frames do interpretador a partir do estado tier 2 e
        retoma a execucao do mais interno para o mais externo.

        O frame mais interno reexecuta a instrucao que falhou. Os frames
        externos ja tinham a chamada em voo, entao retomam na instrucao
        seguinte ao send, com o resultado depositado no registrador de
        destino.
        """
        result = None
        for k in range(len(frames) - 1, -1, -1):
            meth, bci, regs, dest = frames[k]
            regs = list(regs)
            if k != len(frames) - 1 and dest is not None:
                regs[dest] = result
            result = self.interpret(meth, None, None, regs=regs, pc=bci)
        return result

    def interpret(self, m, recv, args, regs=None, pc=0):
        code = m.code
        consts = self.link_consts(m)
        if regs is None:
            regs = [NIL] * m.nregs
            regs[0] = recv
            for i, a in enumerate(args or ()):
                regs[i + 1] = a
        heap = self.heap
        while True:
            ins = code[pc]
            op = ins.op
            if op == SEND:
                a = ins.args
                if len(a) == 0:
                    argv = ()
                elif len(a) == 1:
                    argv = (regs[a[0]],)
                else:
                    argv = tuple(regs[r] for r in a)
                regs[ins.d] = self.send(ins.sel, regs[ins.a], argv, ins.ic)
                pc += 1
            elif op == CONST:
                regs[ins.d] = consts[ins.extra]
                pc += 1
            elif op == MOVE:
                regs[ins.d] = regs[ins.a]
                pc += 1
            elif op == GETF:
                regs[ins.d] = self.get_field(regs[ins.a], ins.b)
                pc += 1
            elif op == SETF:
                self.set_field(regs[ins.a], ins.b, regs[ins.args[0]])
                pc += 1
            elif op == JUMP:
                if ins.target <= pc:
                    m.loop_backedges += 1
                pc = ins.target
            elif op == JFALSE:
                v = regs[ins.a]
                if v == FALSE:
                    pc = ins.target
                elif v == TRUE:
                    pc += 1
                else:
                    raise STError("condicao nao booleana: %s"
                                  % self.class_name(v))
            elif op == JTRUE:
                v = regs[ins.a]
                if v == TRUE:
                    pc = ins.target
                elif v == FALSE:
                    pc += 1
                else:
                    raise STError("condicao nao booleana: %s"
                                  % self.class_name(v))
            elif op == RET:
                return regs[ins.a]
            elif op == NEW:
                regs[ins.d] = self.instantiate(
                    ins.extra, [regs[r] for r in ins.args])
                pc += 1
            elif op == VNEW:
                c = self.ct.get(ins.extra)
                n = dec_int(regs[ins.args[0]])
                ac = self.ct.array_class_for(c)
                regs[ins.d] = heap.alloc_value_array(
                    ac.index, c.index, len(c.fields), n)
                pc += 1
            elif op == ANEW:
                n = dec_int(regs[ins.args[0]])
                regs[ins.d] = heap.alloc_generic_array(CI_ARRAY, n)
                pc += 1
            elif op == CLASSREF:
                regs[ins.d] = self.class_object(ins.extra)
                pc += 1
            elif op == GUARD:
                STATS.guard_checks += 1
                if self.class_index(regs[ins.a]) != ins.extra:
                    STATS.guard_fails += 1
                    raise STError("guard falhou no interpretador")
                pc += 1
            else:
                raise STError("opcode desconhecido %s" % op)
