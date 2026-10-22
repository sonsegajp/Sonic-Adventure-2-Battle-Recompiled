# Calling conventions read off the code, for functions no header declares: what a PC function takes
# (registers live on entry - usercall - and stack slots above its return address, floats where x87
# loads them) and what its GameCube partner takes (r3.., f1.. live on entry), paired by what each
# argument is used for (the struct fields reached through it); where that says nothing, register
# arguments come first, as usercall declarations have them.
import collections
import capstone
from capstone import x86 as X

REG32 = {}
for name, r in (('eax', X.X86_REG_EAX), ('ecx', X.X86_REG_ECX), ('edx', X.X86_REG_EDX), ('ebx', X.X86_REG_EBX),
                ('esi', X.X86_REG_ESI), ('edi', X.X86_REG_EDI), ('ebp', X.X86_REG_EBP)):
    REG32[r] = name
for sub, full in ((X.X86_REG_AL, 'eax'), (X.X86_REG_AH, 'eax'), (X.X86_REG_AX, 'eax'), (X.X86_REG_CL, 'ecx'),
                  (X.X86_REG_CH, 'ecx'), (X.X86_REG_CX, 'ecx'), (X.X86_REG_DL, 'edx'), (X.X86_REG_DH, 'edx'),
                  (X.X86_REG_DX, 'edx'), (X.X86_REG_BL, 'ebx'), (X.X86_REG_BH, 'ebx'), (X.X86_REG_BX, 'ebx'),
                  (X.X86_REG_SI, 'esi'), (X.X86_REG_DI, 'edi'), (X.X86_REG_BP, 'ebp')):
    REG32[sub] = full
CALLEE_SAVED = {'ebx', 'esi', 'edi', 'ebp'}
ARG_REGS = ('eax', 'ecx', 'edx', 'ebx', 'esi', 'edi')
FLOAT_OPS = ('fld', 'fadd', 'fmul', 'fsub', 'fdiv', 'fsubr', 'fdivr', 'fcom', 'fcomp')

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

class PcSig:
    def __init__(self):
        self.regs = []          # register arguments (eax, ecx... order)
        self.stack = {}         # slot -> 'i' / 'f'
        self.pop = 0            # bytes popped by ret N
        self.uses = collections.defaultdict(set)  # argument ('eax' / 's0') -> {(offset, size)}

def _decode(img_bytes, start, end):
    """Reachable instructions from start (within [start, end)): {addr: insn}, successors."""
    insns, succ = {}, {}
    todo = [start]
    while todo:
        a = todo.pop()
        while start <= a < end and a not in insns:
            b = img_bytes(a, 16)
            i = next(md.disasm(b, a, 1), None)
            if i is None: break
            insns[a] = i
            m = i.mnemonic
            nxt = a + i.size
            ops = i.operands
            tgt = ops[0].imm if ops and ops[0].type == X.X86_OP_IMM else None
            if m in ('ret', 'retf', 'int3', 'hlt', 'ud2'):
                succ[a] = []; break
            if m == 'jmp':
                succ[a] = [tgt] if tgt is not None and start <= tgt < end else []
                if succ[a]: todo.append(tgt)
                break
            if m.startswith('j') or m in ('loop', 'loope', 'loopne', 'jecxz'):
                succ[a] = [nxt] + ([tgt] if tgt is not None and start <= tgt < end else [])
                if tgt is not None and start <= tgt < end: todo.append(tgt)
            else:
                succ[a] = [nxt]
            a = nxt
    return insns, succ

def pc_signature(img_bytes, start, end):
    s = PcSig()
    insns, succ = _decode(img_bytes, start, end)
    if not insns: return s
    # pushes of callee-saved registers the function pops again save them (not uses)
    popped = {REG32.get(i.operands[0].reg) for i in insns.values()
              if i.mnemonic == 'pop' and i.operands and i.operands[0].type == X.X86_OP_REG}
    saves = {a for a, i in insns.items()
             if i.mnemonic == 'push' and i.operands and i.operands[0].type == X.X86_OP_REG and
             REG32.get(i.operands[0].reg) in CALLEE_SAVED & popped}
    # per instruction: registers used / defined
    use, dfn = {}, {}
    for a, i in insns.items():
        try: rr, ww = i.regs_access()
        except capstone.CsError: rr, ww = [], []
        u = {REG32[r] for r in rr if r in REG32}
        d = {REG32[r] for r in ww if r in REG32}
        ops = i.operands
        if i.mnemonic in ('xor', 'sub', 'sbb') and len(ops) == 2 and ops[0].type == X.X86_OP_REG and \
                ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg and i.mnemonic != 'sbb':
            u.discard(REG32.get(ops[0].reg))
        if a in saves: u = set()
        if i.mnemonic == 'call': d |= {'eax', 'ecx', 'edx'}
        if i.mnemonic in ('ret', 'retf'):
            u = set()
        use[a], dfn[a] = u, d
    # liveness to a fixed point, instruction granularity
    live = {a: set() for a in insns}
    changed = True
    order = sorted(insns, reverse=True)
    while changed:
        changed = False
        for a in order:
            out = set()
            for n in succ.get(a, []):
                out |= live.get(n, set())
            new = use[a] | (out - dfn[a])
            if new != live[a]: live[a] = new; changed = True
    s.regs = [r for r in ARG_REGS if r in live[start]]
    # stack slots: esp offsets along the code (forward, first visit wins)
    delta, ebpf = {start: 0}, {start: None}
    work = [start]
    while work:
        a = work.pop()
        i = insns[a]; m = i.mnemonic; ops = i.operands
        dd, ef = delta[a], ebpf[a]
        for op in ops:
            if op.type != X.X86_OP_MEM or op.mem.index != 0 or m == 'lea': continue
            k = None
            if op.mem.base == X.X86_REG_ESP: k = op.mem.disp - dd - 4
            elif op.mem.base == X.X86_REG_EBP and ef is not None: k = op.mem.disp - ef - 4
            if k is not None and k >= 0 and k % 4 == 0 and k < 64:
                slot = k // 4
                isf = m in FLOAT_OPS and op.size == 4
                s.stack[slot] = 'f' if (isf or s.stack.get(slot) == 'f') else 'i'
        if m == 'push': dd += 4
        elif m == 'pop': dd -= 4
        elif m in ('sub', 'add') and ops and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP and ops[1].type == X.X86_OP_IMM:
            dd += ops[1].imm if m == 'sub' else -ops[1].imm
        elif m == 'mov' and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_EBP and \
                ops[1].type == X.X86_OP_REG and ops[1].reg == X.X86_REG_ESP:
            ef = dd
        elif m == 'ret' and ops and ops[0].type == X.X86_OP_IMM:
            s.pop = ops[0].imm
        for n in succ.get(a, []):
            if n in insns and n not in delta: delta[n] = dd; ebpf[n] = ef; work.append(n)
    # fields reached through each argument (straight-line from entry: registers keep the argument
    # until written; a stack argument loaded into a register becomes it)
    alias = {r: r for r in s.regs}
    a = start
    seen = set()
    while a in insns and a not in seen:
        seen.add(a)
        i = insns[a]; m = i.mnemonic; ops = i.operands
        for op in ops:
            if op.type == X.X86_OP_MEM and op.mem.index == 0:
                b = REG32.get(op.mem.base)
                if b in alias and 0 <= op.mem.disp < 0x4000: s.uses[alias[b]].add((op.mem.disp, op.size))
        if m == 'mov' and len(ops) == 2 and ops[0].type == X.X86_OP_REG:
            dst = REG32.get(ops[0].reg)
            if ops[1].type == X.X86_OP_REG and REG32.get(ops[1].reg) in alias: alias[dst] = alias[REG32[ops[1].reg]]; a += i.size; continue
            if ops[1].type == X.X86_OP_MEM and ops[1].mem.base == X.X86_REG_ESP and ops[1].mem.index == 0:
                k = ops[1].mem.disp - delta.get(a, 0) - 4
                if k >= 0 and k % 4 == 0: alias[dst] = 's%d' % (k // 4); a += i.size; continue
        for r in dfn[a]: alias.pop(r, None)
        nx = succ.get(a, [])
        if not nx: break
        a = nx[0] if len(nx) == 1 else a + i.size
    return s

# ---- GameCube ----
def _gc_regs(w):
    """(int uses, int defs, float uses, float defs) of one instruction; None for flow."""
    op = w >> 26
    d, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    U, D, FU, FD = set(), set(), set(), set()
    if op in (14, 15):
        if ra: U.add(ra)
        D.add(d)
    elif op in (12, 13, 7, 8): U.add(ra); D.add(d)
    elif op in (10, 11): U.add(ra)
    elif op in (24, 25, 26, 27, 28, 29, 20, 21, 23):
        U.add(d); D.add(ra)
        if op == 20: U.add(ra)
        if op == 23: U.add(rb)
    elif 32 <= op <= 55:
        store = op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55)
        if ra: U.add(ra)
        if op in (46, 47):  # lmw / stmw
            if op == 47: U.update(range(d, 32))
            else: D.update(range(d, 32))
        elif op >= 48: (FU if store else FD).add(d)
        elif store: U.add(d)
        else: D.add(d)
        if op & 1 and op not in (47,) and op != 46: D.add(ra)
    elif op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in (0, 32): U.update((ra, rb))
        elif xo in (23, 55, 87, 119, 279, 311, 343, 375, 534, 790): U.update((ra, rb)); D.add(d)
        elif xo in (151, 183, 215, 247, 407, 439, 662, 918): U.update((d, ra, rb))
        elif xo in (535, 567, 599, 631): U.update((ra, rb)); FD.add(d)
        elif xo in (663, 695, 727, 759, 983): U.update((ra, rb)); FU.add(d)
        elif xo in (339, 371, 83, 19, 595, 659): D.add(d)
        elif xo in (467, 146, 210, 144): U.add(d)
        elif xo in (24, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 954, 922, 26, 986):
            U.update((d, rb)) if xo not in (824, 26, 954, 922, 986) else U.add(d)
            D.add(ra)
        elif xo in (86, 470, 54, 278, 246, 1014, 982, 598, 854): U.update((ra, rb))
        else: U.update((ra, rb)); D.add(d)
    elif op in (59, 63):
        x10 = (w >> 1) & 0x3FF; x5 = (w >> 1) & 0x1F
        fd, fa, fb, fc = d, ra, rb, (w >> 6) & 31
        if op == 63 and x10 in (0, 32): FU.update((fa, fb))
        elif op == 63 and x10 in (72, 40, 264, 136, 12, 14, 15): FU.add(fb); FD.add(fd)
        elif op == 63 and x10 in (583, 711, 70, 38, 64): FD.add(fd) if x10 == 583 else None
        elif x5 == 25: FU.update((fa, fc)); FD.add(fd)
        elif x5 in (28, 29, 30, 31, 23): FU.update((fa, fb, fc)); FD.add(fd)
        else: FU.update((fa, fb)); FD.add(fd)
    elif op == 4:
        FU.update((ra, rb)); FD.add(d)
    elif op in (56, 57, 60, 61):  # psq_l / psq_st
        if ra: U.add(ra)
        (FU if op in (60, 61) else FD).add(d)
        if op in (57, 61): D.add(ra)
    return U, D, FU, FD

def gc_signature(u32, start, end):
    """(int arguments r3..rN as a count, float arguments f1..fM as a count, fields per argument)."""
    words = {}
    for a in range(start, end, 4):
        w = u32(a)
        if w is None: break
        words[a] = w
    succ = {}
    for a, w in words.items():
        op = w >> 26
        if op == 18:
            t = (w & 0x03FFFFFC); t = t - 0x04000000 if t & 0x02000000 else t
            t = (t if w & 2 else a + t) & 0xFFFFFFFF
            if w & 1: succ[a] = [a + 4]
            else: succ[a] = [t] if t in words else []
        elif op == 16:
            t = w & 0xFFFC; t = t - 0x10000 if t & 0x8000 else t
            t = (t if w & 2 else a + t) & 0xFFFFFFFF
            succ[a] = [a + 4] + ([t] if t in words else [])
        elif op == 19 and ((w >> 1) & 0x3FF) in (16, 528):
            bo = (w >> 21) & 31
            succ[a] = [] if (bo & 0x14) == 0x14 else [a + 4]
            if w & 1: succ[a] = [a + 4]  # blrl / bctrl: a call
        else:
            succ[a] = [a + 4] if a + 4 in words else []
    use, dfn = {}, {}
    for a, w in words.items():
        op = w >> 26
        if op == 18 and w & 1 or (op == 19 and w & 1):
            U, D, FU, FD = set(), set(range(0, 13)), set(), set(range(0, 14))
            if op == 19: U = set()
        elif op in (16, 18, 19):
            U, D, FU, FD = set(), set(), set(), set()
        else:
            U, D, FU, FD = _gc_regs(w)
        use[a] = {r for r in U} | {32 + f for f in FU}
        dfn[a] = {r for r in D} | {32 + f for f in FD}
    live = {a: set() for a in words}
    order = sorted(words, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            out = set()
            for n in succ[a]: out |= live.get(n, set())
            new = use[a] | (out - dfn[a])
            if new != live[a]: live[a] = new; changed = True
    entry = live.get(start, set())
    ints = [r for r in range(3, 11) if r in entry]
    flts = [f for f in range(1, 9) if 32 + f in entry]
    nint = max(ints) - 2 if ints else 0
    nflt = max(flts) if flts else 0
    # fields reached through each argument (straight-line from entry)
    uses = collections.defaultdict(set)
    alias = {r: 'r%d' % r for r in ints}
    a = start
    seen = set()
    while a in words and a not in seen:
        seen.add(a)
        w = words[a]; op = w >> 26
        ra = (w >> 16) & 31
        if 32 <= op <= 55 and op not in (46, 47) and ra in alias:
            size = {32: 4, 33: 4, 34: 1, 35: 1, 36: 4, 37: 4, 38: 1, 39: 1, 40: 2, 41: 2, 42: 2, 43: 2, 44: 2, 45: 2,
                    48: 4, 49: 4, 50: 8, 51: 8, 52: 4, 53: 4, 54: 8, 55: 8}[op]
            if (w & 0xFFFF) < 0x4000: uses[alias[ra]].add((w & 0xFFFF, size))
        if op == 31 and ((w >> 1) & 0x3FF) == 444 and ((w >> 21) & 31) == ((w >> 11) & 31):  # mr
            src, dst = (w >> 21) & 31, ra
            if src in alias: alias[dst] = alias[src]; a += 4; continue
        if op == 14 and (w & 0xFFFF) == 0 and ra in alias:  # addi rD, rA, 0
            alias[(w >> 21) & 31] = alias[ra]; a += 4; continue
        for r in dfn[a]:
            if r < 32: alias.pop(r, None)
        nx = succ[a]
        if not nx: break
        a = nx[0]
    return nint, nflt, uses

def pair_args(pcs, nint, nflt, guses):
    """GameCube argument registers from PC sources: [('r3', 'eax'), ('r4', 's0'), ('f1', 's1')], or None."""
    pint = list(pcs.regs) + ['s%d' % k for k in sorted(pcs.stack) if pcs.stack[k] == 'i']
    pflt = ['s%d' % k for k in sorted(pcs.stack) if pcs.stack[k] == 'f']
    if len(pint) != nint or len(pflt) != nflt: return None
    gints = ['r%d' % r for r in range(3, 3 + nint)]
    def sim(p, g):
        a, b = pcs.uses.get(p, set()), guses.get(g, set())
        if not a or not b: return 0.0
        return len(a & b) / len(a | b)
    order = list(range(nint))
    if 1 < nint <= 6:
        import itertools
        base = sum(sim(pint[j], gints[j]) for j in range(nint))
        best = max(((sum(sim(pint[perm[j]], gints[j]) for j in range(nint)), perm) for perm in itertools.permutations(range(nint))),
                   key=lambda t: t[0])
        if best[0] > base + 0.2: order = list(best[1])
    return [(gints[j], pint[order[j]]) for j in range(nint)] + [('f%d' % (k + 1), pflt[k]) for k in range(nflt)]
