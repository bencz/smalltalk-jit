"""
AST -> bytecode de registradores.

Duas coisas acontecem aqui e valem ser destacadas:

1. Blocos de controle (ifTrue:, whileTrue:, to:do:, and:, or:) sao
   inlinados estruturalmente. Nenhum objeto BlockClosure e criado e
   nenhum contexto e reificado. Como o metodo home passa a estar no mesmo
   frame, um retorno nao local vira um simples RET.

2. Aritmetica, at:, at:put: e size NAO sao compilados como primitivas.
   Vao como SEND normal, para que o cache inline colete perfil de tipo.
   O otimizador aprende com o feedback, nao com o compilador trapaceando.
"""

from .front import (Lit, Var, Assign, Send, Ret, Block, MethodDef, ClassDef)
from .bc import (Instr, ICSite, CONST, MOVE, SEND, GETF, SETF, JUMP, JFALSE,
                 JTRUE, RET, NEW, VNEW, ANEW, CLASSREF)
from .model import Method, ClassTable


class Label:
    __slots__ = ("pos",)

    def __init__(self):
        self.pos = None

    def __repr__(self):
        return "L@%s" % self.pos


class CompileError(Exception):
    pass


class MethodCompiler:
    def __init__(self, ct, cls, mdef):
        self.ct = ct
        self.cls = cls
        self.mdef = mdef
        self.m = Method(cls, mdef.sel, mdef.params, mdef.ptypes, mdef.rtype)
        self.code = []
        self.scopes = [{}]
        self.nregs = 0
        self.const_map = {}

    # -------------------------------------------------------- utilitarios
    def alloc_reg(self):
        r = self.nregs
        self.nregs += 1
        return r

    def bind(self, name):
        r = self.alloc_reg()
        self.scopes[-1][name] = r
        return r

    def lookup(self, name):
        for s in reversed(self.scopes):
            if name in s:
                return s[name]
        return None

    def emit(self, ins):
        ins.bci = len(self.code)
        ins.frames = ((self.m, ins.bci),)
        self.code.append(ins)
        return ins

    def const(self, kind, val):
        key = (kind, val)
        if key not in self.const_map:
            self.const_map[key] = len(self.m.consts)
            self.m.consts.append(key)
        return self.const_map[key]

    def new_label(self):
        return Label()

    def place(self, lbl):
        lbl.pos = len(self.code)

    # ----------------------------------------------------------- compilar
    def compile(self):
        self.bind("self")
        for p in self.mdef.params:
            self.bind(p)
        for t in self.mdef.temps:
            r = self.bind(t)
            self.emit(Instr(CONST, d=r, extra=self.const("nil", None)))
        last = self.stmts(self.mdef.body)
        if not (self.code and self.code[-1].op == RET):
            self.emit(Instr(RET, a=0 if last is None else last))
        self.resolve_labels()
        self.m.code = self.code
        self.m.nregs = self.nregs
        return self.m

    def resolve_labels(self):
        for ins in self.code:
            if isinstance(ins.target, Label):
                if ins.target.pos is None:
                    raise CompileError("label nao resolvido")
                ins.target = ins.target.pos

    def stmts(self, body):
        last = None
        for st in body:
            last = self.stmt(st)
        return last

    def stmt(self, node):
        if isinstance(node, Ret):
            r = self.expr(node.expr)
            self.emit(Instr(RET, a=r))
            return r
        return self.expr(node)

    # ------------------------------------------------------- expressoes
    def expr(self, node):
        if isinstance(node, Lit):
            return self.lit(node)
        if isinstance(node, Var):
            return self.var_ref(node)
        if isinstance(node, Assign):
            return self.assign(node)
        if isinstance(node, Send):
            return self.send(node)
        if isinstance(node, Block):
            raise CompileError(
                "bloco em posicao nao inlinavel na linha %d: este prototipo "
                "so aceita blocos literais em ifTrue:/ifFalse:/whileTrue:/"
                "to:do:/timesRepeat:/and:/or:" % node.line)
        raise CompileError("no desconhecido %r" % node)

    def lit(self, node):
        r = self.alloc_reg()
        self.emit(Instr(CONST, d=r, extra=self.const(node.kind, node.val)))
        return r

    def var_ref(self, node):
        r = self.lookup(node.name)
        if r is not None:
            return r
        if node.name in self.cls.fields:
            d = self.alloc_reg()
            self.emit(Instr(GETF, d=d, a=0,
                            b=self.cls.field_index(node.name)))
            return d
        c = self.ct.find(node.name)
        if c is not None:
            d = self.alloc_reg()
            self.emit(Instr(CLASSREF, d=d, extra=c.index))
            return d
        raise CompileError("variavel indefinida: %s (linha %d)"
                           % (node.name, node.line))

    def assign(self, node):
        r = self.lookup(node.name)
        if r is not None:
            src = self.expr(node.expr)
            self.emit(Instr(MOVE, d=r, a=src))
            return r
        if node.name in self.cls.fields:
            if self.cls.is_value() and self.mdef.sel != "__init__":
                raise CompileError(
                    "classe de valor e imutavel: nao se pode atribuir a %s"
                    % node.name)
            src = self.expr(node.expr)
            self.emit(Instr(SETF, a=0,
                            b=self.cls.field_index(node.name), args=[src]))
            return src
        raise CompileError("atribuicao a variavel indefinida: %s (linha %d)"
                           % (node.name, node.line))

    # ------------------------------------------------------------- sends
    def send(self, node):
        sel = node.sel
        # 1. formas de controle inlinadas
        h = INLINED.get(sel)
        if h is not None and self.all_literal_blocks(node, sel):
            return h(self, node)
        # 2. receptor e uma referencia de classe conhecida
        if isinstance(node.recv, Var) and self.lookup(node.recv.name) is None \
                and node.recv.name not in self.cls.fields:
            c = self.ct.find(node.recv.name)
            if c is not None:
                r = self.class_send(c, node)
                if r is not None:
                    return r
        # 3. envio comum, com sitio de cache inline
        recv = self.expr(node.recv)
        args = [self.expr(a) for a in node.args]
        d = self.alloc_reg()
        ic = ICSite(sel)
        self.m.ics.append(ic)
        self.emit(Instr(SEND, d=d, a=recv, args=args, sel=sel, ic=ic))
        return d

    def all_literal_blocks(self, node, sel):
        need = BLOCK_ARGS.get(sel, ())
        for i in need:
            if i == -1:
                if not isinstance(node.recv, Block):
                    return False
            else:
                if i >= len(node.args) or not isinstance(node.args[i], Block):
                    return False
        return True

    def class_send(self, c, node):
        sel = node.sel
        if sel == "new" and not c.fields:
            d = self.alloc_reg()
            self.emit(Instr(NEW, d=d, extra=c.index, args=[]))
            return d
        if sel == "new" and c.fields and not c.is_value():
            d = self.alloc_reg()
            self.emit(Instr(NEW, d=d, extra=c.index, args=[]))
            return d
        if sel == "new:" and c.name == "Array":
            n = self.expr(node.args[0])
            d = self.alloc_reg()
            self.emit(Instr(ANEW, d=d, args=[n]))
            return d
        if sel == "arrayNew:":
            if not (c.is_value() and c.all_float_fields()):
                raise CompileError(
                    "arrayNew: exige classe Value com todos os campos "
                    "Float64 (layout plano); %s nao qualifica" % c.name)
            n = self.expr(node.args[0])
            d = self.alloc_reg()
            self.emit(Instr(VNEW, d=d, extra=c.index, args=[n]))
            return d
        # construtor por palavras-chave casando com os campos
        if sel.endswith(":") and c.fields:
            parts = [p for p in sel.split(":") if p]
            if parts == c.fields:
                args = [self.expr(a) for a in node.args]
                d = self.alloc_reg()
                self.emit(Instr(NEW, d=d, extra=c.index, args=args))
                return d
        return None

    # ------------------------------------------- formas de controle inline
    def block_value(self, blk, argregs=()):
        """Compila o corpo de um bloco literal no frame corrente."""
        self.scopes.append({})
        for name, r in zip(blk.params, argregs):
            self.scopes[-1][name] = r
        for t in blk.temps:
            r = self.bind(t)
            self.emit(Instr(CONST, d=r, extra=self.const("nil", None)))
        v = self.stmts(blk.body)
        self.scopes.pop()
        if v is None:
            v = self.alloc_reg()
            self.emit(Instr(CONST, d=v, extra=self.const("nil", None)))
        return v

    def c_ifTrue(self, node):
        return self._cond(node, node.args[0], None)

    def c_ifFalse(self, node):
        return self._cond(node, None, node.args[0])

    def c_ifTrueIfFalse(self, node):
        return self._cond(node, node.args[0], node.args[1])

    def c_ifFalseIfTrue(self, node):
        return self._cond(node, node.args[1], node.args[0])

    def _cond(self, node, tblk, fblk):
        c = self.expr(node.recv)
        d = self.alloc_reg()
        lelse, lend = self.new_label(), self.new_label()
        self.emit(Instr(JFALSE, a=c, target=lelse))
        v = self.block_value(tblk) if tblk is not None else self.nil_reg()
        self.emit(Instr(MOVE, d=d, a=v))
        self.emit(Instr(JUMP, target=lend))
        self.place(lelse)
        v = self.block_value(fblk) if fblk is not None else self.nil_reg()
        self.emit(Instr(MOVE, d=d, a=v))
        self.place(lend)
        return d

    def nil_reg(self):
        r = self.alloc_reg()
        self.emit(Instr(CONST, d=r, extra=self.const("nil", None)))
        return r

    def c_whileTrue(self, node):
        lhead, lend = self.new_label(), self.new_label()
        self.place(lhead)
        c = self.block_value(node.recv)
        self.emit(Instr(JFALSE, a=c, target=lend))
        if node.args:
            self.block_value(node.args[0])
        self.emit(Instr(JUMP, target=lhead))
        self.place(lend)
        return self.nil_reg()

    def c_whileFalse(self, node):
        lhead, lend = self.new_label(), self.new_label()
        self.place(lhead)
        c = self.block_value(node.recv)
        self.emit(Instr(JTRUE, a=c, target=lend))
        if node.args:
            self.block_value(node.args[0])
        self.emit(Instr(JUMP, target=lhead))
        self.place(lend)
        return self.nil_reg()

    def c_toDo(self, node):
        return self._todo(node, node.recv, node.args[0], None, node.args[1])

    def c_toByDo(self, node):
        return self._todo(node, node.recv, node.args[0], node.args[1],
                          node.args[2])

    def _todo(self, node, startn, stopn, stepn, blk):
        start = self.expr(startn)
        limit_src = self.expr(stopn)
        limit = self.alloc_reg()
        self.emit(Instr(MOVE, d=limit, a=limit_src))
        step = None
        if stepn is not None:
            s = self.expr(stepn)
            step = self.alloc_reg()
            self.emit(Instr(MOVE, d=step, a=s))
        ivar = self.alloc_reg()
        self.emit(Instr(MOVE, d=ivar, a=start))
        one = self.alloc_reg()
        self.emit(Instr(CONST, d=one, extra=self.const("int", 1)))
        lhead, lend = self.new_label(), self.new_label()
        self.place(lhead)
        c = self.alloc_reg()
        ic = ICSite("<=")
        self.m.ics.append(ic)
        self.emit(Instr(SEND, d=c, a=ivar, args=[limit], sel="<=", ic=ic))
        self.emit(Instr(JFALSE, a=c, target=lend))
        self.block_value(blk, [ivar])
        nxt = self.alloc_reg()
        ic2 = ICSite("+")
        self.m.ics.append(ic2)
        self.emit(Instr(SEND, d=nxt, a=ivar, args=[step or one], sel="+",
                        ic=ic2))
        self.emit(Instr(MOVE, d=ivar, a=nxt))
        self.emit(Instr(JUMP, target=lhead))
        self.place(lend)
        return start

    def c_timesRepeat(self, node):
        limit_src = self.expr(node.recv)
        limit = self.alloc_reg()
        self.emit(Instr(MOVE, d=limit, a=limit_src))
        ivar = self.alloc_reg()
        self.emit(Instr(CONST, d=ivar, extra=self.const("int", 1)))
        one = self.alloc_reg()
        self.emit(Instr(CONST, d=one, extra=self.const("int", 1)))
        lhead, lend = self.new_label(), self.new_label()
        self.place(lhead)
        c = self.alloc_reg()
        ic = ICSite("<=")
        self.m.ics.append(ic)
        self.emit(Instr(SEND, d=c, a=ivar, args=[limit], sel="<=", ic=ic))
        self.emit(Instr(JFALSE, a=c, target=lend))
        self.block_value(node.args[0])
        nxt = self.alloc_reg()
        ic2 = ICSite("+")
        self.m.ics.append(ic2)
        self.emit(Instr(SEND, d=nxt, a=ivar, args=[one], sel="+", ic=ic2))
        self.emit(Instr(MOVE, d=ivar, a=nxt))
        self.emit(Instr(JUMP, target=lhead))
        self.place(lend)
        return limit

    def c_and(self, node):
        c = self.expr(node.recv)
        d = self.alloc_reg()
        lfalse, lend = self.new_label(), self.new_label()
        self.emit(Instr(MOVE, d=d, a=c))
        self.emit(Instr(JFALSE, a=c, target=lend))
        v = self.block_value(node.args[0])
        self.emit(Instr(MOVE, d=d, a=v))
        self.place(lfalse)
        self.place(lend)
        return d

    def c_or(self, node):
        c = self.expr(node.recv)
        d = self.alloc_reg()
        lend = self.new_label()
        self.emit(Instr(MOVE, d=d, a=c))
        self.emit(Instr(JTRUE, a=c, target=lend))
        v = self.block_value(node.args[0])
        self.emit(Instr(MOVE, d=d, a=v))
        self.place(lend)
        return d


INLINED = {
    "ifTrue:": MethodCompiler.c_ifTrue,
    "ifFalse:": MethodCompiler.c_ifFalse,
    "ifTrue:ifFalse:": MethodCompiler.c_ifTrueIfFalse,
    "ifFalse:ifTrue:": MethodCompiler.c_ifFalseIfTrue,
    "whileTrue:": MethodCompiler.c_whileTrue,
    "whileFalse:": MethodCompiler.c_whileFalse,
    "to:do:": MethodCompiler.c_toDo,
    "to:by:do:": MethodCompiler.c_toByDo,
    "timesRepeat:": MethodCompiler.c_timesRepeat,
    "and:": MethodCompiler.c_and,
    "or:": MethodCompiler.c_or,
}

# indice dos argumentos que precisam ser blocos literais (-1 = receptor)
BLOCK_ARGS = {
    "ifTrue:": (0,),
    "ifFalse:": (0,),
    "ifTrue:ifFalse:": (0, 1),
    "ifFalse:ifTrue:": (0, 1),
    "whileTrue:": (-1, 0),
    "whileFalse:": (-1, 0),
    "to:do:": (1,),
    "to:by:do:": (2,),
    "timesRepeat:": (0,),
    "and:": (0,),
    "or:": (0,),
}


def compile_program(classdefs):
    ct = ClassTable()
    for cd in classdefs:
        c = ct.define(cd.name, cd.kind)
        c.fields = list(cd.fields)
        c.ftypes = dict(cd.ftypes)
        if c.is_value():
            for f in c.fields:
                if f not in c.ftypes:
                    raise CompileError(
                        "campo %s de classe Value %s precisa de anotacao de "
                        "tipo para ter layout plano" % (f, c.name))
    for cd in classdefs:
        c = ct.find(cd.name)
        for md in cd.methods:
            m = MethodCompiler(ct, c, md).compile()
            ct.register_method(m)
    return ct
