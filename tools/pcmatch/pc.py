# PC executable analysis for the matcher: functions (recursive descent from header seeds, call
# targets and code pointers in data), and per function the features that survive recompilation
# for another CPU: strings, float constants, struct offsets, integer constants, globals, callees.
import collections, struct
import capstone, pefile
from capstone import x86 as X

class PcFunc:
    __slots__ = ('addr', 'insns', 'calls', 'strings', 'floats', 'offsets', 'consts', 'globals', 'end', 'jumps',
                 'callsites', 'events')
    def __init__(self, addr):
        self.addr = addr; self.insns = 0; self.calls = []; self.callsites = []; self.strings = []; self.floats = []
        self.offsets = []; self.consts = []; self.globals = []; self.end = addr; self.jumps = 0
        self.events = []  # in address order: ('s', str) ('c', const) ('f', float) ('call', target, at)

class PcExe:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.img = pe.get_memory_mapped_image()
        self.secs = {}
        for s in pe.sections:
            n = s.Name.rstrip(b'\0').decode('latin1')
            self.secs[n] = (self.base + s.VirtualAddress, self.base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
        self.text = self.secs['.text']
        self.relocs = set()
        for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in blk.entries:
                if e.type == 3: self.relocs.add(self.base + e.rva)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.funcs = {}

    def in_text(self, a): return self.text[0] <= a < self.text[1]
    def in_sec(self, a, n): return n in self.secs and self.secs[n][0] <= a < self.secs[n][1]
    def u32(self, a):
        o = a - self.base
        return struct.unpack_from('<I', self.img, o)[0] if 0 <= o < len(self.img) - 3 else None
    def f32(self, a):
        o = a - self.base
        return struct.unpack_from('<f', self.img, o)[0] if 0 <= o < len(self.img) - 3 else None
    def f64(self, a):
        o = a - self.base
        return struct.unpack_from('<d', self.img, o)[0] if 0 <= o < len(self.img) - 7 else None
    def cstr(self, a, minlen=3):
        o = a - self.base
        if not (0 <= o < len(self.img)): return None
        e = self.img.find(b'\0', o, o + 512)
        if e < 0 or e - o < minlen: return None
        s = self.img[o:e]
        if all(32 <= c < 127 or c in (9, 10, 13) for c in s): return s.decode('latin1')
        return None

    def code_pointers(self):
        """Pointers into .text stored in data (vtables, callback tables): function starts."""
        out = set()
        text = sorted(r for r in self.relocs if self.in_text(r))
        tset = set(text)
        for r in self.relocs:
            v = self.u32(r)
            if v is None or not self.in_text(v): continue
            if self.in_text(r):
                # In code: an immediate (push offset f, mov [x], offset f) unless part of a jump
                # table (three or more pointers in a row).
                run = sum(1 for k in (-8, -4, 4, 8) if r + k in tset and self.in_text(self.u32(r + k) or 0))
                if run >= 2: continue
            out.add(v)
        return out

    def decode(self, a):
        o = a - self.base
        for i in self.md.disasm(self.img[o:o + 16], a, 1):
            return i
        return None

    def analyze(self, seeds):
        work = collections.deque(sorted(set(seeds) | self.code_pointers()))
        known = set(work)
        while work:
            a = work.popleft()
            if a in self.funcs or not self.in_text(a): continue
            f = self.function(a, known)
            self.funcs[a] = f
            for t in f.calls:
                if t not in known and self.in_text(t): known.add(t); work.append(t)
        return self.funcs

    def function(self, start, starts):
        f = PcFunc(start)
        seen = set(); todo = [start]
        while todo:
            a = todo.pop()
            while a not in seen and self.in_text(a):
                if a != start and a in starts and a in self.funcs: break  # ran into another function
                i = self.decode(a)
                if i is None: break
                seen.add(a); f.insns += 1
                f.end = max(f.end, a + i.size)
                self.features(f, i)
                m = i.mnemonic
                if m == 'call':
                    if i.operands[0].type == X.X86_OP_IMM:
                        t = i.operands[0].imm; f.calls.append(t); f.callsites.append((a, t)); f.events.append(('call', t, a))
                    a += i.size; continue
                if m in ('ret', 'retf', 'int3', 'hlt', 'ud2'): break
                if m == 'jmp':
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm
                        if t in starts and t != start: f.calls.append(t); f.callsites.append((a, t)); break  # tail call
                        a = t; continue
                    if op.type == X.X86_OP_MEM and op.mem.disp and op.mem.index != 0 and op.mem.base == 0:
                        tab = op.mem.disp & 0xFFFFFFFF  # jump table: reloc'd entries into .text
                        k = 0
                        while tab + 4 * k in self.relocs:
                            t = self.u32(tab + 4 * k)
                            if not self.in_text(t): break
                            todo.append(t); k += 1
                        f.jumps += k
                    break
                if m.startswith('j') or m in ('loop', 'loope', 'loopne', 'jecxz'):
                    if i.operands and i.operands[0].type == X.X86_OP_IMM: todo.append(i.operands[0].imm)
                a += i.size
        return f

    def features(self, f, i):
        # Absolute addresses: the bytes of the instruction holding a relocation.
        for k in range(i.size - 3):
            if i.address + k in self.relocs:
                v = self.u32(i.address + k)
                if v is None: continue
                if self.in_text(v):  # a function's address (a callback set, a table)
                    if i.mnemonic not in ('call', 'jmp'): f.events.append(('p', v, i.address))
                    continue
                s = self.cstr(v)
                if s is not None: f.strings.append(s); f.events.append(('s', s, i.address)); continue
                size = 0
                for op in i.operands:
                    if op.type == X.X86_OP_MEM and (op.mem.disp & 0xFFFFFFFF) == v: size = op.size
                if self.in_sec(v, '.rdata') and size in (4, 8):
                    x = self.f32(v) if size == 4 else self.f64(v)
                    if i.mnemonic.startswith('f') or 'ss' in i.mnemonic or 'sd' in i.mnemonic or size == 8:
                        if x is not None and x == x:
                            x = float(struct.unpack('<f', struct.pack('<f', x))[0]) if abs(x) < 3e38 else x
                            f.floats.append(x); f.events.append(('f', x, i.address)); continue
                f.globals.append((v, size, i.mnemonic, i.address))
        for k, op in enumerate(i.operands):
            if op.type == X.X86_OP_MEM and op.mem.base not in (0, X.X86_REG_ESP) and op.mem.index == 0:
                d = op.mem.disp
                if 4 <= d < 0x4000:
                    f.offsets.append(d)
                    # a store when the memory operand is the destination (first operand) of a writing
                    # instruction (mov, fstp, add [m], ...), else a load
                    store = k == 0 and i.mnemonic not in ('cmp', 'test', 'push', 'fld', 'fild', 'fcom', 'fcomp',
                                                          'fadd', 'fmul', 'fsub', 'fdiv', 'fsubr', 'fdivr', 'comiss',
                                                          'ucomiss', 'movss' if len(i.operands) > 1 and i.operands[1].type == X.X86_OP_MEM else '')
                    if i.mnemonic in ('fst', 'fstp', 'fistp', 'fist'): store = True
                    f.events.append(('o', d, op.size, store, i.address))
            elif op.type == X.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                if not any(i.address + k in self.relocs for k in range(i.size - 3)) and i.mnemonic not in ('call', 'jmp') \
                        and not i.mnemonic.startswith('j') and v not in (0, 1, 0xFFFFFFFF):
                    f.consts.append(v); f.events.append(('c', v, i.address))
