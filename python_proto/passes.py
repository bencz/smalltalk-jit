"""
Passes de otimizacao sobre a IR em SSA.

A ordem importa mais que os passes individuais:

  1. phis triviais       -> limpa o ruido da construcao de SSA
  2. propagacao de tipos -> guards tornam os tipos exatos
  3. guards redundantes  -> um guard dominado por outro equivalente sai
  4. substituicao escalar-> objetos que nao escapam viram registradores
  5. GVN                 -> aqui e que box(unbox(x)) desaparece
  6. promocao de phi     -> tira o boxing carregado pelo laco
  7. DCE                 -> remove a alocacao agora sem usos

Nada disso e valido sem os mapas de desotimizacao. Quando a substituicao
escalar apaga um objeto, ela deixa no lugar uma receita de materializacao,
para que o interpretador possa reconstrui-lo se um guard falhar.
"""

from .ir import (Value, Materialize, TAGGED, F64, I64, BOOL, VOID, PURE)
from .model import (CI_SMALLINT, CI_FLOAT, CI_BOXEDFLOAT, CI_TRUE, CI_FALSE)
from .value import is_int, is_float, dec_int, dec_float

REMOVABLE_IF_UNUSED = {"newv", "new", "vnew", "anew"}
SIDE_EFFECTS = {"setfieldt", "setfieldf", "vastore", "astore", "send",
                "guard_class", "ret", "jump", "branch"}


class PassStats:
    def __init__(self):
        self.trivial_phis = 0
        self.guards_removed = 0
        self.scalar_replaced = 0
        self.materializations = 0
        self.gvn_removed = 0
        self.box_pairs_removed = 0
        self.phis_promoted = 0
        self.dead_removed = 0
        self.hoisted = 0
        self.blocks_merged = 0
        self.allocs_remaining = 0


# ------------------------------------------------------------- utilitarios

def all_values(fn):
    for b in fn.blocks:
        for v in b.phis:
            yield v
        for v in b.instrs:
            yield v
        if b.term is not None:
            yield b.term


def deopt_iter(fn):
    for v in all_values(fn):
        if v.deopt:
            for fr in v.deopt:
                yield v, fr


def replace_all_uses(fn, old, new):
    if old is new:
        return
    for v in all_values(fn):
        for i, a in enumerate(v.args):
            if a is old:
                v.args[i] = new
        if v.deopt:
            for fr in v.deopt:
                for reg, s in list(fr.slots.items()):
                    if s is old:
                        fr.slots[reg] = new
                    elif isinstance(s, Materialize):
                        for j, f in enumerate(s.fields):
                            if f is old:
                                s.fields[j] = new


def use_map(fn):
    uses = {}
    for v in all_values(fn):
        for a in v.args:
            if isinstance(a, Value):
                uses.setdefault(a.id, []).append(v)
    return uses


def deopt_only_users(fn, target):
    """Valores que referenciam `target` somente em estados de desotimizacao."""
    hard = 0
    for v in all_values(fn):
        for a in v.args:
            if a is target:
                hard += 1
    return hard == 0


def remove_from_block(v):
    b = v.block
    if b is None:
        return
    if v.op == "phi":
        if v in b.phis:
            b.phis.remove(v)
    elif v in b.instrs:
        b.instrs.remove(v)


def insert_before(block, ref, v):
    v.block = block
    if ref is None or ref not in block.instrs:
        block.instrs.append(v)
    else:
        block.instrs.insert(block.instrs.index(ref), v)
    return v


# ------------------------------------------------------- 1. phis triviais

def remove_trivial_phis(fn, st):
    changed = True
    while changed:
        changed = False
        for b in list(fn.blocks):
            for phi in list(b.phis):
                same = None
                trivial = True
                for a in phi.args:
                    if a is phi or a is same:
                        continue
                    if same is not None:
                        trivial = False
                        break
                    same = a
                if not trivial:
                    continue
                if same is None:
                    continue
                b.phis.remove(phi)
                replace_all_uses(fn, phi, same)
                st.trivial_phis += 1
                changed = True
    return st


# ---------------------------------------------------- 2. propagacao de tipos

def type_prop(fn, st):
    changed = True
    while changed:
        changed = False
        for v in all_values(fn):
            k = v.klass
            if v.op == "box_f":
                nk = CI_FLOAT
            elif v.op == "box_i":
                nk = CI_SMALLINT
            elif v.op == "newv" or v.op == "new":
                nk = v.extra
            elif v.op == "phi":
                ks = set()
                for a in v.args:
                    if isinstance(a, Value):
                        ks.add(a.klass)
                nk = ks.pop() if len(ks) == 1 else None
            elif v.op == "const":
                nk = v.klass
            else:
                nk = v.klass
            if nk is not None and nk != k:
                v.klass = nk
                changed = True
    return st


# -------------------------------------------- 3. guards redundantes / simplify

def simplify(fn, st):
    """
    Reescritas locais. Duas familias importam:

      - guard_class(v, C) some quando o tipo de v ja e C, seja porque
        outro guard dominante ja estabeleceu isso, seja porque v e o
        resultado de uma alocacao.
      - fieldf(newv(...), k) vira diretamente o k-esimo argumento. E este
        casamento que faz a substituicao escalar funcionar.
    """
    changed = False
    for b in fn.blocks:
        known = {}
        for v in list(b.instrs):
            if v.op == "guard_class":
                a = v.args[0]
                if a.klass == v.extra or known.get(a.id) == v.extra:
                    b.instrs.remove(v)
                    st.guards_removed += 1
                    changed = True
                    continue
                # nao se anota a.klass aqui: isso faria a proxima
                # iteracao concluir que este mesmo guard e redundante
                known[a.id] = v.extra
            elif v.op in ("fieldf", "fieldt"):
                src = v.args[0]
                if src.op in ("newv", "new") and v.extra < len(src.args):
                    repl = src.args[v.extra]
                    if repl.repr == v.repr:
                        replace_all_uses(fn, v, repl)
                        b.instrs.remove(v)
                        st.scalar_replaced += 1
                        changed = True
            elif v.op == "unbox_f":
                src = v.args[0]
                if src.op == "box_f":
                    replace_all_uses(fn, v, src.args[0])
                    b.instrs.remove(v)
                    st.box_pairs_removed += 1
                    changed = True
                elif src.op == "const" and is_float(src.konst):
                    nv = Value("fconst", konst=dec_float(src.konst))
                    insert_before(b, v, nv)
                    replace_all_uses(fn, v, nv)
                    b.instrs.remove(v)
                    changed = True
                elif src.op == "const" and is_int(src.konst):
                    nv = Value("fconst", konst=float(dec_int(src.konst)))
                    insert_before(b, v, nv)
                    replace_all_uses(fn, v, nv)
                    b.instrs.remove(v)
                    changed = True
            elif v.op == "unbox_i":
                src = v.args[0]
                if src.op == "box_i":
                    replace_all_uses(fn, v, src.args[0])
                    b.instrs.remove(v)
                    st.box_pairs_removed += 1
                    changed = True
                elif src.op == "const" and is_int(src.konst):
                    nv = Value("iconst", konst=dec_int(src.konst))
                    insert_before(b, v, nv)
                    replace_all_uses(fn, v, nv)
                    b.instrs.remove(v)
                    changed = True
            elif v.op == "box_f":
                src = v.args[0]
                if src.op == "unbox_f":
                    replace_all_uses(fn, v, src.args[0])
                    b.instrs.remove(v)
                    st.box_pairs_removed += 1
                    changed = True
            elif v.op == "i2f":
                src = v.args[0]
                if src.op == "iconst":
                    nv = Value("fconst", konst=float(src.konst))
                    insert_before(b, v, nv)
                    replace_all_uses(fn, v, nv)
                    b.instrs.remove(v)
                    changed = True
            elif v.op == "tag2bool":
                src = v.args[0]
                if src.op == "bool2tag":
                    replace_all_uses(fn, v, src.args[0])
                    b.instrs.remove(v)
                    changed = True
    return changed


# ------------------------------------------------------------------ 4. GVN

def gvn(fn, st):
    """
    Numeracao global de valores para operacoes puras, em ordem de
    dominancia aproximada (RPO). Suficiente para o efeito que interessa:
    colapsar cargas de campo e conversoes repetidas.
    """
    table = {}
    changed = False
    for b in fn.rpo():
        for v in list(b.instrs):
            if v.op not in PURE:
                continue
            key = (v.op, v.extra, v.konst,
                   tuple(a.id for a in v.args))
            prev = table.get(key)
            if prev is not None:
                replace_all_uses(fn, v, prev)
                b.instrs.remove(v)
                st.gvn_removed += 1
                changed = True
            else:
                table[key] = v
    return changed


# ------------------------------------- 5. substituicao escalar / materializacao

def scalar_replacement(fn, st):
    """
    Um objeto cujos unicos usos restantes sao estados de desotimizacao nao
    precisa existir. Trocamos o valor por uma receita de materializacao:
    classe, mais a lista de valores SSA que preenchem os campos.
    """
    changed = False
    for b in fn.blocks:
        for v in list(b.instrs):
            if v.op not in ("newv", "new"):
                continue
            if not deopt_only_users(fn, v):
                continue
            used_in_deopt = False
            for holder, fr in deopt_iter(fn):
                for reg, s in list(fr.slots.items()):
                    if s is v:
                        used_in_deopt = True
            flat = (v.op == "newv")
            if used_in_deopt:
                mat = Materialize(v.extra, list(v.args), flat)
                for holder, fr in deopt_iter(fn):
                    for reg, s in list(fr.slots.items()):
                        if s is v:
                            fr.slots[reg] = mat
                st.materializations += 1
            b.instrs.remove(v)
            st.scalar_replaced += 1
            changed = True
    return changed


# --------------------------------------------- 6. promocao de representacao

BOXOP = {F64: "box_f", I64: "box_i"}
UNBOXOP = {F64: "unbox_f", I64: "unbox_i"}


def promote_phis(fn, st):
    """
    Promocao de representacao dos phis carregados pelo laco.

    Um acumulador como `total := total + x` nasce como um phi TAGGED cujo
    unico produtor e um box_f e cujo unico consumidor e um unbox_f. Se
    promovermos o phi para F64, o par desaparece e o acumulador vive em
    xmm por todo o laco. Sem este passe, todo o resto da eliminacao de
    boxing para na fronteira do laco.
    """
    phis = [v for v in all_values(fn) if v.op == "phi" and v.repr == TAGGED]
    if not phis:
        return False
    idx = {p.id: p for p in phis}

    # componentes conexas do grafo de phis
    comp = {}
    comps = []
    for p in phis:
        if p.id in comp:
            continue
        stack, group = [p], []
        comp[p.id] = len(comps)
        while stack:
            q = stack.pop()
            group.append(q)
            neigh = [a for a in q.args if isinstance(a, Value)
                     and a.id in idx]
            for other in phis:
                if other is q:
                    continue
                if q in other.args and other.id not in comp:
                    neigh.append(other)
            for nb in neigh:
                if nb.id not in comp:
                    comp[nb.id] = len(comps)
                    stack.append(nb)
        comps.append(group)

    uses = use_map(fn)
    changed = False
    for group in comps:
        gset = set(g.id for g in group)
        target = None
        ok = True
        for p in group:
            for a in p.args:
                if a.id in gset:
                    continue
                if a.op in ("box_f",):
                    target = target or F64
                    if target != F64:
                        ok = False
                elif a.op in ("box_i",):
                    target = target or I64
                    if target != I64:
                        ok = False
                elif a.op == "const" and is_float(a.konst):
                    target = target or F64
                    if target != F64:
                        ok = False
                elif a.op == "const" and is_int(a.konst):
                    if target is None:
                        target = I64
                    elif target == F64:
                        pass
                    elif target != I64:
                        ok = False
                else:
                    ok = False
                if not ok:
                    break
            if not ok:
                break
        if not ok or target is None:
            continue

        # promove
        for p in group:
            p.repr = target
            p.klass = CI_FLOAT if target == F64 else CI_SMALLINT
            st.phis_promoted += 1
            changed = True
        # ajusta entradas
        for p in group:
            for i, a in enumerate(p.args):
                if a.id in gset:
                    continue
                if a.op == BOXOP[target]:
                    p.args[i] = a.args[0]
                elif a.op == "const":
                    if target == F64:
                        k = (dec_float(a.konst) if is_float(a.konst)
                             else float(dec_int(a.konst)))
                        nv = Value("fconst", konst=k)
                    else:
                        nv = Value("iconst", konst=dec_int(a.konst))
                    fn.entry.instrs.insert(0, nv)
                    nv.block = fn.entry
                    p.args[i] = nv
                elif a.op == "box_i" and target == F64:
                    conv = Value("i2f", [a.args[0]])
                    pred = p.block.preds[i]
                    insert_before(pred, pred.instrs[-1] if pred.instrs
                                  else None, conv)
                    p.args[i] = conv
        # ajusta consumidores
        for p in group:
            for u in list(uses.get(p.id, ())):
                if u.id in gset:
                    continue
                if u.op == UNBOXOP[target]:
                    replace_all_uses(fn, u, p)
                    remove_from_block(u)
                    st.box_pairs_removed += 1
                else:
                    needs_tag = any(a is p for a in u.args)
                    if not needs_tag:
                        continue
                    if u.repr in (F64, I64, BOOL) and u.op in (
                            "fadd", "fsub", "fmul", "fdiv", "fcmp",
                            "iadd", "isub", "imul", "icmp", "i2f",
                            "vaload", "vastore", "aload", "astore"):
                        continue
                    bx = Value(BOXOP[target], [p])
                    bx.klass = CI_FLOAT if target == F64 else CI_SMALLINT
                    host = u.block
                    if u.op == "phi":
                        continue
                    insert_before(host, u, bx)
                    u.replace_arg(p, bx)
    return changed


# ------------------------------------------------------------------ 7. DCE

def dce(fn, st):
    live = set()
    work = []
    for v in all_values(fn):
        if v.op in SIDE_EFFECTS or v.repr == VOID:
            live.add(v.id)
            work.append(v)
    while work:
        v = work.pop()
        for a in v.args:
            if isinstance(a, Value) and a.id not in live:
                live.add(a.id)
                work.append(a)
        if v.deopt:
            for fr in v.deopt:
                for reg, s in fr.slots.items():
                    if isinstance(s, Value) and s.id not in live:
                        live.add(s.id)
                        work.append(s)
                    elif isinstance(s, Materialize):
                        for f in s.fields:
                            if isinstance(f, Value) and f.id not in live:
                                live.add(f.id)
                                work.append(f)
    for b in fn.blocks:
        for v in list(b.instrs):
            if v.id not in live:
                b.instrs.remove(v)
                st.dead_removed += 1
        for v in list(b.phis):
            if v.id not in live:
                b.phis.remove(v)
                st.dead_removed += 1
    return st


# ---------------------------------------------------------------- pipeline

def optimize(fn, rounds=6):
    st = PassStats()
    remove_trivial_phis(fn, st)
    type_prop(fn, st)
    for _ in range(rounds):
        changed = False
        changed |= bool(simplify(fn, st))
        changed |= bool(gvn(fn, st))
        type_prop(fn, st)
        changed |= bool(remove_trivial_phis(fn, st) and False)
        changed |= bool(scalar_replacement(fn, st))
        dce(fn, st)
        if not changed:
            break
    licm(fn, st)
    promote_phis(fn, st)
    for _ in range(rounds):
        changed = False
        changed |= bool(simplify(fn, st))
        changed |= bool(gvn(fn, st))
        changed |= bool(scalar_replacement(fn, st))
        dce(fn, st)
        remove_trivial_phis(fn, st)
        if not changed:
            break
    licm(fn, st)
    dce(fn, st)
    merge_blocks(fn, st)
    st.allocs_remaining = sum(
        1 for v in all_values(fn) if v.op in ("newv", "new", "vnew", "anew"))
    fn.pass_stats = st
    return st


# ------------------------------------------------ 8. movimentacao de codigo

def dominators(fn):
    order = fn.rpo()
    pos = {b.id: i for i, b in enumerate(order)}
    idom = {fn.entry.id: fn.entry}

    def intersect(a, b):
        while a is not b:
            while pos[a.id] > pos[b.id]:
                a = idom[a.id]
            while pos[b.id] > pos[a.id]:
                b = idom[b.id]
        return a

    changed = True
    while changed:
        changed = False
        for b in order:
            if b is fn.entry:
                continue
            new = None
            for p in b.preds:
                if p.id not in idom:
                    continue
                new = p if new is None else intersect(new, p)
            if new is not None and idom.get(b.id) is not new:
                idom[b.id] = new
                changed = True
    return idom, pos


def dominates(idom, a, b):
    cur = b
    while True:
        if cur is a:
            return True
        nxt = idom.get(cur.id)
        if nxt is None or nxt is cur:
            return False
        cur = nxt


def find_loops(fn):
    idom, pos = dominators(fn)
    loops = []
    for b in fn.blocks:
        for s in b.succs:
            if dominates(idom, s, b):
                body = {s.id, b.id}
                stack = [b]
                while stack:
                    x = stack.pop()
                    for p in x.preds:
                        if p.id not in body:
                            body.add(p.id)
                            stack.append(p)
                outside = [p for p in s.preds if p.id not in body]
                pre = outside[0] if len(outside) == 1 else None
                loops.append((s, body, pre))
    loops.sort(key=lambda t: -len(t[1]))
    return loops, idom


def licm(fn, st):
    """
    Hoisting de operacoes puras invariantes do laco.

    Sem este passe, uma leitura de campo como `data` continua sendo feita
    a cada iteracao, e a promessa de "nenhum acesso tagged dentro do laco"
    nao se sustenta. Aplicado repetidamente, tira a operacao de um laco
    interno para o externo e do externo para fora.
    """
    changed = False
    for _ in range(6):
        loops, idom = find_loops(fn)
        moved_any = False
        for header, body, pre in loops:
            if pre is None:
                continue
            defs_in = set()
            for b in fn.blocks:
                if b.id in body:
                    for v in b.phis + b.instrs:
                        defs_in.add(v.id)
            for b in fn.blocks:
                if b.id not in body:
                    continue
                for v in list(b.instrs):
                    if v.op not in PURE or v.op == "phi":
                        continue
                    if any(isinstance(a, Value) and a.id in defs_in
                           for a in v.args):
                        continue
                    b.instrs.remove(v)
                    pre.instrs.append(v)
                    v.block = pre
                    st.hoisted = getattr(st, "hoisted", 0) + 1
                    moved_any = True
                    changed = True
        if not moved_any:
            break
    return changed


# -------------------------------------------------- 9. fusao de blocos

def merge_blocks(fn, st):
    """
    O inlining deixa fronteiras de metodo como blocos que so contem um
    jump. Fundi-los nao muda semantica e deixa o resultado legivel.
    """
    changed = True
    while changed:
        changed = False
        for b in list(fn.blocks):
            if b not in fn.blocks or len(b.succs) != 1:
                continue
            s = b.succs[0]
            if s is b or s is fn.entry or len(s.preds) != 1 or s.phis:
                continue
            b.instrs.extend(s.instrs)
            for v in s.instrs:
                v.block = b
            b.term = s.term
            if b.term is not None:
                b.term.block = b
            b.succs = list(s.succs)
            for x in s.succs:
                x.preds = [b if p is s else p for p in x.preds]
            fn.blocks.remove(s)
            st.blocks_merged = getattr(st, "blocks_merged", 0) + 1
            changed = True
    return changed
