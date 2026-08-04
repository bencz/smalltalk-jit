"""
Tabela de classes e objetos-metodo.

O indice de classe vive no cabecalho do objeto, nao um ponteiro. Isso faz
o guard de um cache inline ser um cmp de 32 bits contra imediato, sem
tocar uma segunda linha de cache.
"""

# Indices de classe reservados para os builtins.
CI_UNDEFINED = 0
CI_FALSE = 1
CI_TRUE = 2
CI_SMALLINT = 3
CI_FLOAT = 4          # SmallFloat imediato
CI_BOXEDFLOAT = 5     # Float que nao coube na janela imediata
CI_CHAR = 6
CI_ARRAY = 7          # Array generico (tagged)
CI_VALUEARRAY = 8     # Array de classe de valor, layout plano
CI_STRING = 9
CI_SYMBOL = 10
CI_CLASS = 11
N_BUILTIN = 12

BUILTIN_NAMES = {
    CI_UNDEFINED: "UndefinedObject",
    CI_FALSE: "False",
    CI_TRUE: "True",
    CI_SMALLINT: "SmallInteger",
    CI_FLOAT: "Float",
    CI_BOXEDFLOAT: "BoxedFloat",
    CI_CHAR: "Character",
    CI_ARRAY: "Array",
    CI_VALUEARRAY: "ValueArray",
    CI_STRING: "String",
    CI_SYMBOL: "Symbol",
    CI_CLASS: "Class",
}

# Tipos escalares reconhecidos em anotacoes.
SCALAR_TYPES = {"Float64", "Int64", "Boolean"}


class Method:
    def __init__(self, cls, sel, params, ptypes, rtype):
        self.cls = cls
        self.sel = sel
        self.params = params
        self.ptypes = ptypes
        self.rtype = rtype
        self.code = []
        self.nregs = 0
        self.consts = []          # (kind, pyvalue)
        self.ics = []
        self.calls = 0            # contador de invocacao (tiering)
        self.loop_backedges = 0   # contador de backedge (OSR)
        self.optimized = None     # codigo tier 2 instalado
        self.deopt_count = 0
        self.inlinable = True
        # Depois de desotimizar demais, este metodo passa a exigir
        # caches estritamente monomorficos para voltar a especular.
        self.speculation_burned = False

    def full_name(self):
        return "%s>>%s" % (self.cls.name if self.cls else "?", self.sel)

    def __repr__(self):
        return "<Method %s>" % self.full_name()


class ClassInfo:
    def __init__(self, index, name, kind="Object"):
        self.index = index
        self.name = name
        self.kind = kind          # "Object" | "Value" | "Builtin"
        self.fields = []
        self.ftypes = {}          # nome -> "Float64" | "Int64" | classe
        self.methods = {}         # seletor -> Method
        self.subclasses = []
        self.array_of = None      # se for ValueArray(C), aponta para C
        self.array_class = None   # se for C, aponta para ValueArray(C)
        self.field_is_f64 = []
        self.flat = False

    def is_value(self):
        return self.kind == "Value"

    def field_index(self, name):
        return self.fields.index(name)

    def all_float_fields(self):
        return (bool(self.fields)
                and all(self.ftypes.get(f) == "Float64" for f in self.fields))

    def __repr__(self):
        return "<Class %s#%d>" % (self.name, self.index)


class ClassTable:
    def __init__(self):
        self.classes = []
        self.by_name = {}
        for i in range(N_BUILTIN):
            c = ClassInfo(i, BUILTIN_NAMES[i], "Builtin")
            self.classes.append(c)
            self.by_name[c.name] = c
        # Analise de hierarquia de classes: seletor -> lista de metodos.
        self.implementors = {}

    def define(self, name, kind):
        if name in self.by_name and self.by_name[name].kind != "Builtin":
            raise ValueError("classe redefinida: %s" % name)
        c = ClassInfo(len(self.classes), name, kind)
        self.classes.append(c)
        self.by_name[name] = c
        return c

    def get(self, index):
        return self.classes[index]

    def array_class_for(self, c):
        """
        Cada `ValueArray of: C` e uma classe propria, com seu indice.
        Sem isso o cache inline diria apenas "e um ValueArray" e o
        otimizador nao saberia o layout do elemento.
        """
        if c.array_class is None:
            ac = ClassInfo(len(self.classes), "ValueArray(%s)" % c.name,
                           "Builtin")
            ac.array_of = c
            ac.field_is_f64 = []
            self.classes.append(ac)
            self.by_name[ac.name] = ac
            c.array_class = ac
        return c.array_class

    def find(self, name):
        return self.by_name.get(name)

    def register_method(self, m):
        m.cls.methods[m.sel] = m
        self.implementors.setdefault(m.sel, []).append(m)

    # ------------------------------------------------- analise de hierarquia
    def unique_implementor(self, sel):
        """
        Se um seletor tem exatamente um implementador em todo o sistema,
        o inlining pode dispensar o guard, desde que registre a dependencia
        e seja invalidado na instalacao de um segundo implementador.
        """
        impls = self.implementors.get(sel, ())
        return impls[0] if len(impls) == 1 else None
