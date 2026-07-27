"""
Front end: lexer e parser.

Sintaxe no estilo GNU Smalltalk, com duas extensoes:

    Vec3 := Value [ ... ]        classes de valor (imutaveis, sem identidade)
    | x::Float64 y::Float64 |    anotacao de tipo em campos
    dot: o::Vec3 -> Float64 [ ]  anotacao em parametros e retorno

As anotacoes sao opcionais em toda parte, exceto em campos de classes
Value, onde sao o que viabiliza o layout plano.
"""

# ------------------------------------------------------------------ lexer

BINCHARS = set("+-*/\\~<>=&|@%,?!")

KEYWORDS_RESERVED = {"self", "super", "nil", "true", "false", "thisContext"}


class Tok:
    __slots__ = ("kind", "val", "line")

    def __init__(self, kind, val, line):
        self.kind = kind
        self.val = val
        self.line = line

    def __repr__(self):
        return "Tok(%s,%r)" % (self.kind, self.val)


def lex(src):
    toks = []
    i = 0
    n = len(src)
    line = 1
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if c == '"':                      # comentario
            i += 1
            while i < n and src[i] != '"':
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 1
            continue
        if c == "'":                      # string
            i += 1
            buf = []
            while i < n and src[i] != "'":
                buf.append(src[i])
                i += 1
            i += 1
            toks.append(Tok("str", "".join(buf), line))
            continue
        if c == "#":
            i += 1
            j = i
            while j < n and (src[j].isalnum() or src[j] in "_:"):
                j += 1
            toks.append(Tok("sym", src[i:j], line))
            i = j
            continue
        if c.isdigit() or (
            c == "-"
            and i + 1 < n
            and src[i + 1].isdigit()
            and (not toks or toks[-1].kind in ("bin", "kw", "lpar", "lbrack",
                                               "dot", "caret", "assign",
                                               "bar", "semi", "arrow"))
        ):
            j = i
            if src[j] == "-":
                j += 1
            while j < n and src[j].isdigit():
                j += 1
            isf = False
            if j < n and src[j] == "." and j + 1 < n and src[j + 1].isdigit():
                isf = True
                j += 1
                while j < n and src[j].isdigit():
                    j += 1
            if j < n and src[j] in "eE":
                k = j + 1
                if k < n and src[k] in "+-":
                    k += 1
                if k < n and src[k].isdigit():
                    isf = True
                    j = k
                    while j < n and src[j].isdigit():
                        j += 1
            text = src[i:j]
            toks.append(Tok("float" if isf else "int",
                            float(text) if isf else int(text), line))
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            name = src[i:j]
            # keyword message part: ident seguido de ':' que nao e '::' nem ':='
            if (j < n and src[j] == ":"
                    and not (j + 1 < n and src[j + 1] in ":=")):
                toks.append(Tok("kw", name + ":", line))
                i = j + 1
            else:
                toks.append(Tok("id", name, line))
                i = j
            continue
        if src.startswith("::", i):
            toks.append(Tok("dcolon", "::", line))
            i += 2
            continue
        if src.startswith(":=", i):
            toks.append(Tok("assign", ":=", line))
            i += 2
            continue
        if src.startswith("->", i):
            toks.append(Tok("arrow", "->", line))
            i += 2
            continue
        if c == ":":
            toks.append(Tok("colon", ":", line))
            i += 1
            continue
        if c == "(":
            toks.append(Tok("lpar", "(", line)); i += 1; continue
        if c == ")":
            toks.append(Tok("rpar", ")", line)); i += 1; continue
        if c == "[":
            toks.append(Tok("lbrack", "[", line)); i += 1; continue
        if c == "]":
            toks.append(Tok("rbrack", "]", line)); i += 1; continue
        if c == ".":
            toks.append(Tok("dot", ".", line)); i += 1; continue
        if c == ";":
            toks.append(Tok("semi", ";", line)); i += 1; continue
        if c == "^":
            toks.append(Tok("caret", "^", line)); i += 1; continue
        if c == "|":
            toks.append(Tok("bar", "|", line)); i += 1; continue
        if c in BINCHARS:
            j = i
            while j < n and src[j] in BINCHARS and src[j] != "|":
                j += 1
            toks.append(Tok("bin", src[i:j], line))
            i = j
            continue
        raise SyntaxError("caractere inesperado %r na linha %d" % (c, line))
    toks.append(Tok("eof", None, line))
    return toks


# -------------------------------------------------------------------- AST

class Node:
    __slots__ = ("line",)


class Lit(Node):
    __slots__ = ("kind", "val")

    def __init__(self, kind, val, line=0):
        self.kind = kind
        self.val = val
        self.line = line


class Var(Node):
    __slots__ = ("name",)

    def __init__(self, name, line=0):
        self.name = name
        self.line = line


class Assign(Node):
    __slots__ = ("name", "expr")

    def __init__(self, name, expr, line=0):
        self.name = name
        self.expr = expr
        self.line = line


class Send(Node):
    __slots__ = ("recv", "sel", "args", "is_super")

    def __init__(self, recv, sel, args, line=0, is_super=False):
        self.recv = recv
        self.sel = sel
        self.args = args
        self.line = line
        self.is_super = is_super


class Ret(Node):
    __slots__ = ("expr",)

    def __init__(self, expr, line=0):
        self.expr = expr
        self.line = line


class Block(Node):
    __slots__ = ("params", "temps", "body")

    def __init__(self, params, temps, body, line=0):
        self.params = params
        self.temps = temps
        self.body = body
        self.line = line


class MethodDef:
    def __init__(self, sel, params, ptypes, rtype, temps, body, line=0):
        self.sel = sel
        self.params = params
        self.ptypes = ptypes
        self.rtype = rtype
        self.temps = temps
        self.body = body
        self.line = line


class ClassDef:
    def __init__(self, name, kind, fields, ftypes, methods, line=0):
        self.name = name
        self.kind = kind            # "Object" ou "Value"
        self.fields = fields
        self.ftypes = ftypes
        self.methods = methods
        self.line = line


# ----------------------------------------------------------------- parser

class Parser:
    def __init__(self, src):
        self.toks = lex(src)
        self.p = 0

    def peek(self, k=0):
        return self.toks[min(self.p + k, len(self.toks) - 1)]

    def next(self):
        t = self.toks[self.p]
        self.p += 1
        return t

    def at(self, kind, val=None):
        t = self.peek()
        return t.kind == kind and (val is None or t.val == val)

    def expect(self, kind, val=None):
        t = self.next()
        if t.kind != kind or (val is not None and t.val != val):
            raise SyntaxError("esperava %s%s, achei %s %r na linha %d"
                              % (kind, "" if val is None else " " + val,
                                 t.kind, t.val, t.line))
        return t

    # -------------------------------------------------------- top level
    def parse_program(self):
        classes = []
        while not self.at("eof"):
            classes.append(self.parse_classdef())
        return classes

    def parse_classdef(self):
        line = self.peek().line
        name = self.expect("id").val
        self.expect("assign")
        kind = self.expect("id").val
        if kind not in ("Object", "Value"):
            raise SyntaxError("superclasse suportada: Object ou Value, achei %s"
                              % kind)
        self.expect("lbrack")
        fields, ftypes = [], {}
        if self.at("bar"):
            fields, ftypes = self.parse_var_decls()
        methods = []
        while not self.at("rbrack"):
            methods.append(self.parse_method())
        self.expect("rbrack")
        return ClassDef(name, kind, fields, ftypes, methods, line)

    def parse_var_decls(self):
        self.expect("bar")
        names, types = [], {}
        while self.at("id"):
            nm = self.next().val
            names.append(nm)
            if self.at("dcolon"):
                self.next()
                types[nm] = self.expect("id").val
        self.expect("bar")
        return names, types

    def parse_method(self):
        line = self.peek().line
        t = self.peek()
        params, ptypes = [], {}
        if t.kind == "id":
            sel = self.next().val
        elif t.kind == "bin" or t.kind == "bar":
            sel = self.next().val
            pn = self.expect("id").val
            params.append(pn)
            if self.at("dcolon"):
                self.next()
                ptypes[pn] = self.expect("id").val
        elif t.kind == "kw":
            parts = []
            while self.at("kw"):
                parts.append(self.next().val)
                pn = self.expect("id").val
                params.append(pn)
                if self.at("dcolon"):
                    self.next()
                    ptypes[pn] = self.expect("id").val
            sel = "".join(parts)
        else:
            raise SyntaxError("padrao de metodo invalido na linha %d" % t.line)
        rtype = None
        if self.at("arrow"):
            self.next()
            rtype = self.expect("id").val
        self.expect("lbrack")
        temps = []
        if self.at("bar"):
            temps, _ = self.parse_var_decls()
        body = self.parse_statements("rbrack")
        self.expect("rbrack")
        return MethodDef(sel, params, ptypes, rtype, temps, body, line)

    # ------------------------------------------------------- statements
    def parse_statements(self, endkind):
        stmts = []
        while not self.at(endkind) and not self.at("eof"):
            if self.at("caret"):
                line = self.next().line
                stmts.append(Ret(self.parse_expr(), line))
            else:
                stmts.append(self.parse_expr())
            if self.at("dot"):
                self.next()
            else:
                break
        return stmts

    def parse_expr(self):
        if (self.at("id") and self.peek(1).kind == "assign"
                and self.peek().val not in KEYWORDS_RESERVED):
            name = self.next().val
            line = self.next().line
            return Assign(name, self.parse_expr(), line)
        return self.parse_keyword()

    def parse_keyword(self):
        recv = self.parse_binary()
        if not self.at("kw"):
            return recv
        parts, args = [], []
        line = self.peek().line
        while self.at("kw"):
            parts.append(self.next().val)
            args.append(self.parse_binary())
        return Send(recv, "".join(parts), args, line,
                    isinstance(recv, Var) and recv.name == "super")

    def parse_binary(self):
        left = self.parse_unary()
        while self.at("bin") or (self.at("bar") and self._bar_is_binop()):
            t = self.next()
            right = self.parse_unary()
            left = Send(left, t.val, [right], t.line,
                        isinstance(left, Var) and left.name == "super")
        return left

    def _bar_is_binop(self):
        return self.peek(1).kind in ("id", "int", "float", "lpar", "str")

    def parse_unary(self):
        recv = self.parse_primary()
        while self.at("id") and self.peek(1).kind != "assign":
            t = self.next()
            recv = Send(recv, t.val, [], t.line,
                        isinstance(recv, Var) and recv.name == "super")
        return recv

    def parse_primary(self):
        t = self.peek()
        if t.kind == "int":
            self.next(); return Lit("int", t.val, t.line)
        if t.kind == "float":
            self.next(); return Lit("float", t.val, t.line)
        if t.kind == "str":
            self.next(); return Lit("str", t.val, t.line)
        if t.kind == "sym":
            self.next(); return Lit("sym", t.val, t.line)
        if t.kind == "id":
            self.next()
            if t.val == "nil":
                return Lit("nil", None, t.line)
            if t.val == "true":
                return Lit("bool", True, t.line)
            if t.val == "false":
                return Lit("bool", False, t.line)
            return Var(t.val, t.line)
        if t.kind == "lpar":
            self.next()
            e = self.parse_expr()
            self.expect("rpar")
            return e
        if t.kind == "lbrack":
            return self.parse_block()
        raise SyntaxError("expressao inesperada: %s %r na linha %d"
                          % (t.kind, t.val, t.line))

    def parse_block(self):
        line = self.expect("lbrack").line
        params = []
        if self.at("colon"):
            while self.at("colon"):
                self.next()
                params.append(self.expect("id").val)
            self.expect("bar")
        temps = []
        if self.at("bar"):
            temps, _ = self.parse_var_decls()
        body = self.parse_statements("rbrack")
        self.expect("rbrack")
        return Block(params, temps, body, line)


def parse(src):
    return Parser(src).parse_program()
