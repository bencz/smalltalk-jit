"""
Modelo de valores com tagging de 64 bits.

Esquema (bit 0 == 0 para SmallInteger, para que o guard de aritmetica
seja um simples OR + TEST):

    bit0 == 0                -> SmallInteger, valor = sar(w, 1)
    bit0 == 1, bits 2..1:
        00  (tag 001)        -> ponteiro de heap, indice = w >> 3
        01  (tag 011)        -> Character
        10  (tag 101)        -> imediato (nil / true / false)
        11  (tag 111)        -> SmallFloat (expoente rotacionado, estilo Spur)

Tudo aqui e instrumentado: cada alocacao, cada box, cada unbox e cada
leitura tagged incrementa um contador. Sao esses contadores, e nao o
relogio de parede de um prototipo em Python, que validam as afirmacoes
sobre eliminacao de boxing.
"""

import struct

M64 = (1 << 64) - 1

TAG_PTR = 0b001
TAG_CHAR = 0b011
TAG_IMM = 0b101
TAG_FLOAT = 0b111

SMALLINT_MIN = -(1 << 62)
SMALLINT_MAX = (1 << 62) - 1

NIL = (0 << 3) | TAG_IMM
FALSE = (1 << 3) | TAG_IMM
TRUE = (2 << 3) | TAG_IMM


class Stats:
    """Contadores de instrumentacao. Um por execucao."""

    FIELDS = (
        "allocs", "boxes", "unboxes", "tagged_loads", "tagged_ops",
        "sends", "ic_hits", "ic_misses", "guard_checks", "guard_fails",
        "deopts", "f64_ops", "i64_ops", "field_loads",
    )

    def __init__(self):
        self.reset()

    def reset(self):
        for f in self.FIELDS:
            setattr(self, f, 0)

    def snapshot(self):
        return {f: getattr(self, f) for f in self.FIELDS}

    def diff(self, before):
        return {f: getattr(self, f) - before[f] for f in self.FIELDS}


STATS = Stats()


# ---------------------------------------------------------------- inteiros

def enc_int(v):
    if not (SMALLINT_MIN <= v <= SMALLINT_MAX):
        raise OverflowError("SmallInteger overflow: %d" % v)
    return (v << 1) & M64


def dec_int(w):
    w = w & M64
    if w & (1 << 63):
        w -= 1 << 64
    return w >> 1


def is_int(w):
    return (w & 1) == 0


def both_int(a, b):
    """O guard de aritmetica inteira: um OR e um TEST."""
    STATS.guard_checks += 1
    return ((a | b) & 1) == 0


# ------------------------------------------------------------------ floats
#
# 1 bit de sinal + 8 bits de expoente + 52 de mantissa = 61 bits, que e
# exatamente o que sobra com uma tag de 3 bits. A janela de expoente
# escolhida e [0x380, 0x47F]; 0x380 fica reservado para codificar o zero.

_EXP_LO = 0x380
_EXP_HI = 0x47F


def float_fits(d):
    bits = struct.unpack("<Q", struct.pack("<d", d))[0]
    exp = (bits >> 52) & 0x7FF
    if bits == 0:
        return True
    return _EXP_LO < exp <= _EXP_HI


def enc_float(d, heap=None):
    """Codifica um double imediato; se nao couber, aloca no heap."""
    bits = struct.unpack("<Q", struct.pack("<d", d))[0]
    if bits == 0:
        return (0 << 3) | TAG_FLOAT
    exp = (bits >> 52) & 0x7FF
    if _EXP_LO < exp <= _EXP_HI:
        sign = bits >> 63
        mant = bits & ((1 << 52) - 1)
        payload = (sign << 60) | ((exp - _EXP_LO) << 52) | mant
        return (payload << 3) | TAG_FLOAT
    if heap is None:
        raise OverflowError("float fora da janela imediata e sem heap: %r" % d)
    return heap.alloc_boxed_float(d)


def dec_float(w):
    payload = w >> 3
    e8 = (payload >> 52) & 0xFF
    mant = payload & ((1 << 52) - 1)
    if e8 == 0 and mant == 0:
        return 0.0
    sign = (payload >> 60) & 1
    bits = (sign << 63) | ((e8 + _EXP_LO) << 52) | mant
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def is_float(w):
    return (w & 0b111) == TAG_FLOAT


def is_ptr(w):
    return (w & 0b111) == TAG_PTR


# ------------------------------------------------------------------- heap

class Obj:
    """Objeto de heap. class_index no cabecalho, nao ponteiro de classe."""

    __slots__ = ("class_index", "fields", "forward")

    def __init__(self, class_index, fields):
        self.class_index = class_index
        self.fields = fields
        self.forward = None


class ValueArray:
    """
    Array de classe de valor com layout plano.

    N elementos de K campos Float64 viram uma unica lista de N*K floats
    crus. Nao ha ponteiro por elemento, nao ha cabecalho por elemento e
    nao ha indirecao ao percorrer.
    """

    __slots__ = ("class_index", "elem_class_index", "nfields", "count", "data")

    def __init__(self, class_index, elem_class_index, nfields, count):
        self.class_index = class_index
        self.elem_class_index = elem_class_index
        self.nfields = nfields
        self.count = count
        self.data = [0.0] * (count * nfields)


class GenericArray:
    __slots__ = ("class_index", "data")

    def __init__(self, class_index, count):
        self.class_index = class_index
        self.data = [NIL] * count


class BoxedFloat:
    __slots__ = ("class_index", "value")

    def __init__(self, class_index, value):
        self.class_index = class_index
        self.value = value


class Heap:
    def __init__(self, boxedfloat_class_index):
        self.objects = [None]
        self.boxedfloat_class_index = boxedfloat_class_index

    def _push(self, o):
        STATS.allocs += 1
        self.objects.append(o)
        return ((len(self.objects) - 1) << 3) | TAG_PTR

    def alloc(self, class_index, fields):
        return self._push(Obj(class_index, list(fields)))

    def alloc_value_array(self, class_index, elem_ci, nfields, count):
        return self._push(ValueArray(class_index, elem_ci, nfields, count))

    def alloc_generic_array(self, class_index, count):
        return self._push(GenericArray(class_index, count))

    def alloc_boxed_float(self, d):
        STATS.boxes += 1
        return self._push(BoxedFloat(self.boxedfloat_class_index, d))

    def deref(self, w):
        STATS.tagged_loads += 1
        return self.objects[w >> 3]

    def deref_raw(self, w):
        return self.objects[w >> 3]
