# GameCube side for the matcher: main.dol and every REL, the RELs linked at synthetic addresses
# (relocations applied, as OSLink would) so references resolve; functions from the recompiler's
# tables (build/gen); per function the same features as pc.py.
import glob, os, re, struct

R13, R2 = 0x803B52C0, 0x803B6520
REL_BASE = 0x90000000  # synthetic: module i at REL_BASE + i * REL_STRIDE
REL_STRIDE = 0x00800000

def sext16(v): return v - 0x10000 if v & 0x8000 else v

class Region:
    def __init__(self, start, data, name, module, section, code):
        self.start, self.data, self.name, self.module, self.section, self.code = start, bytearray(data), name, module, section, code
    @property
    def end(self): return self.start + len(self.data)

class GcFunc:
    __slots__ = ('addr', 'end', 'module', 'insns', 'calls', 'strings', 'floats', 'offsets', 'consts', 'globals',
                 'callsites', 'rel', 'name', 'events', 'hash')
    def __init__(self, addr, end, module, rel):
        self.addr, self.end, self.module, self.rel = addr, end, module, rel
        self.insns = 0; self.calls = []; self.strings = []; self.floats = []; self.offsets = []; self.consts = []
        self.globals = []; self.callsites = []; self.name = None; self.events = []; self.hash = 0

class GcGame:
    def __init__(self, root='.'):
        self.regions = []
        self.branch = {}    # bl/b patched by a relocation -> its target (the synthetic addresses are
                            # too far from main.dol for the 24-bit offset)
        self.modbase = {}   # module id -> {section: address}
        self.modfile = {}
        self.load_dol(os.path.join(root, 'orig/main.dol'))
        self.load_rels(os.path.join(root, 'orig/rel'), os.path.join(root, 'build/gen/rel_index.cpp'))
        self.regions.sort(key=lambda r: r.start)
        self.funcs = {}

    def load_dol(self, path):
        d = open(path, 'rb').read()
        o = struct.unpack_from('>18I', d, 0); a = struct.unpack_from('>18I', d, 0x48); s = struct.unpack_from('>18I', d, 0x90)
        for i in range(18):
            if s[i]: self.regions.append(Region(a[i], d[o[i]:o[i] + s[i]], 'dol%d' % i, 0, i, i < 7))
        bss_a, bss_s = struct.unpack_from('>2I', d, 0xD8)
        # bss: the parts not covered by the (small) data sections
        self.regions.append(Region(bss_a, b'\0' * bss_s, 'bss', 0, 99, False))

    def load_rels(self, reldir, index):
        files = {}
        for l in open(index, encoding='utf-8'):
            m = re.match(r'\s*\{(\d+)u, "([^"]+)"', l)
            if m: files[int(m.group(1))] = m.group(2)
        mods = {}
        for k, (mid, fname) in enumerate(sorted(files.items())):
            path = os.path.join(reldir, fname)
            if not os.path.exists(path): continue
            b = open(path, 'rb').read()
            nsec, secoff = struct.unpack_from('>II', b, 12)
            bss = struct.unpack_from('>I', b, 32)[0]
            base = REL_BASE + k * REL_STRIDE
            cur = base
            secs = {}
            for i in range(nsec):
                off, size = struct.unpack_from('>II', b, secoff + 8 * i)
                if not size: continue
                cur = (cur + 31) & ~31
                code = bool(off & 1); off &= ~1
                data = b[off:off + size] if off else b'\0' * size  # off 0: the bss section
                secs[i] = cur
                self.regions.append(Region(cur, data, '%s:%d' % (fname, i), mid, i, code))
                cur += size
            self.modbase[mid] = secs
            self.modfile[mid] = fname
            mods[mid] = b
        # relocations
        for mid, b in mods.items():
            impoff, impsz = struct.unpack_from('>II', b, 40)
            for k in range(impsz // 8):
                imod, ioff = struct.unpack_from('>II', b, impoff + 8 * k)
                if imod != 0 and imod not in self.modbase: continue
                sec, pos, o = None, 0, ioff
                while True:
                    off, t, s, add = struct.unpack_from('>HBBI', b, o); o += 8
                    pos += off
                    if t == 203: break
                    if t == 202: sec, pos = s, 0; continue
                    if t == 201 or sec not in self.modbase[mid]: continue
                    target = add if imod == 0 else self.modbase[imod].get(s, 0) + add
                    self.relocate(self.modbase[mid][sec] + pos, t, target)

    def region(self, a):
        lo, hi = 0, len(self.regions)
        while lo < hi:
            m = (lo + hi) // 2
            r = self.regions[m]
            if a < r.start: hi = m
            elif a >= r.end: lo = m + 1
            else: return r
        return None
    def u32(self, a):
        r = self.region(a)
        return struct.unpack_from('>I', r.data, a - r.start)[0] if r and a + 4 <= r.end else None
    def put32(self, a, v):
        r = self.region(a)
        if r and a + 4 <= r.end: struct.pack_into('>I', r.data, a - r.start, v & 0xFFFFFFFF)
    def put16(self, a, v):
        r = self.region(a)
        if r and a + 2 <= r.end: struct.pack_into('>H', r.data, a - r.start, v & 0xFFFF)
    def relocate(self, p, t, s):
        if t == 1: self.put32(p, s)
        elif t == 4: self.put16(p, s)
        elif t == 5: self.put16(p, s >> 16)
        elif t == 6: self.put16(p, (s + 0x8000) >> 16)
        elif t == 3: self.put16(p, s)
        elif t == 10:
            w = self.u32(p); self.put32(p, (w & ~0x03FFFFFC) | ((s - p) & 0x03FFFFFC))
            self.branch[p] = s
        elif t == 11:
            w = self.u32(p); self.put32(p, (w & ~0xFFFC) | ((s - p) & 0xFFFC))
        elif t == 2:
            w = self.u32(p); self.put32(p, (w & ~0x03FFFFFC) | (s & 0x03FFFFFC))
    def f32(self, a):
        w = self.u32(a)
        return struct.unpack('>f', struct.pack('>I', w))[0] if w is not None else None
    def f64(self, a):
        w0, w1 = self.u32(a), self.u32(a + 4)
        if w0 is None or w1 is None: return None
        return struct.unpack('>d', struct.pack('>II', w0, w1))[0]
    def cstr(self, a, minlen=3):
        r = self.region(a)
        if not r or r.code: return None
        o = a - r.start
        e = r.data.find(b'\0', o, o + 512)
        if e < 0 or e - o < minlen: return None
        s = bytes(r.data[o:e])
        if all(32 <= c < 127 or c in (9, 10, 13) for c in s): return s.decode('latin1')
        return None
    def is_data(self, a):
        r = self.region(a)
        return r is not None and not r.code

    def load_funcs(self, gen='build/gen'):
        for l in open(os.path.join(gen, 'dol_table.cpp'), encoding='utf-8'):
            m = re.match(r'\s*\{0x([0-9A-F]+)u, &\w+, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\}', l)
            if m:
                a, body, end = (int(x, 16) for x in m.groups())
                if a == body: self.funcs[a] = GcFunc(a, end, 0, a)
        for mid, fname in self.modfile.items():
            base = 'rel_' + fname.split('.')[0]
            p = os.path.join(gen, base + '_table.cpp')
            if not os.path.exists(p): continue
            for l in open(p, encoding='utf-8'):
                m = re.match(r'\s*\{0x([0-9A-F]+)u, &\w+, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\}', l)
                if not m: continue
                a, body, end = (int(x, 16) for x in m.groups())
                if a != body: continue
                secs = self.modbase[mid]
                sa = secs.get(a >> 24)
                if sa is None: continue
                real = sa + (a & 0xFFFFFF)
                self.funcs[real] = GcFunc(real, sa + (end & 0xFFFFFF), mid, a)
        for f in self.funcs.values(): self.features(f)
        return self.funcs

    def code_hash(self, f):
        """The code with what linking changes masked (branch targets, 16-bit immediates that may be
        address halves): copies of one function in several modules hash the same."""
        import hashlib
        h = hashlib.blake2b(digest_size=8)
        for a in range(f.addr, f.end, 4):
            w = self.u32(a)
            if w is None: break
            op = w >> 26
            if op == 18: w &= 0xFC000003
            elif op == 16: w &= 0xFFFF0003
            elif op in (14, 15, 24, 25) or 32 <= op <= 55: w &= 0xFFFF0000
            h.update(w.to_bytes(4, 'big'))
        return int.from_bytes(h.digest(), 'big')

    def features(self, f):
        f.hash = self.code_hash(f)
        known = {13: R13, 2: R2, 0: None}
        for a in range(f.addr, f.end, 4):
            w = self.u32(a)
            if w is None: break
            f.insns += 1
            op = w >> 26; d = (w >> 21) & 31; ra = (w >> 16) & 31; imm = w & 0xFFFF
            if op == 18:
                t = (w & 0x03FFFFFC); t = t - 0x04000000 if t & 0x02000000 else t
                t = (t if w & 2 else a + t) & 0xFFFFFFFF
                t = self.branch.get(a, t)
                if w & 1:
                    f.calls.append(t); f.callsites.append((a, t)); f.events.append(('call', t, a))
                    for r in range(3, 13): known[r] = None
                elif not (f.addr <= t < f.end):
                    f.calls.append(t); f.callsites.append((a, t))  # tail call
                continue
            if op == 15:  # lis / addis
                known[d] = (imm << 16) & 0xFFFFFFFF if ra == 0 else (known.get(ra) + (imm << 16)) & 0xFFFFFFFF if known.get(ra) is not None else None
                continue
            if op == 14:  # addi / li
                if ra == 0:
                    v = sext16(imm); known[d] = v & 0xFFFFFFFF
                    if v not in (0, 1, -1): f.consts.append(v & 0xFFFFFFFF); f.events.append(('c', v & 0xFFFFFFFF, a))
                elif known.get(ra) is not None:
                    v = (known[ra] + sext16(imm)) & 0xFFFFFFFF; known[d] = v
                    self.address(f, v, 0, a)
                else:
                    known[d] = None
                continue
            if op == 24:  # ori
                known[ra] = (known[d] | imm) if known.get(d) is not None else None
                if known[ra] is not None and not self.region(known[ra]) and known[ra] not in (0, 1, 0xFFFFFFFF):
                    f.consts.append(known[ra]); f.events.append(('c', known[ra], a))
                continue
            if op in (10, 11):  # cmpli / cmpi
                v = imm if op == 10 else sext16(imm) & 0xFFFFFFFF
                if v not in (0, 1, 0xFFFFFFFF): f.consts.append(v); f.events.append(('c', v, a))
                continue
            if 32 <= op <= 55:
                size = {32: 4, 33: 4, 34: 1, 35: 1, 36: 4, 37: 4, 38: 1, 39: 1, 40: 2, 41: 2, 42: 2, 43: 2, 44: 2, 45: 2,
                        46: 4, 47: 4, 48: 4, 49: 4, 50: 8, 51: 8, 52: 4, 53: 4, 54: 8, 55: 8}[op]
                base = known.get(ra) if ra else None
                if ra and base is not None:
                    ea = (base + sext16(imm)) & 0xFFFFFFFF
                    if op in (48, 49, 50, 51) and self.is_data(ea):
                        x = self.f32(ea) if size == 4 else self.f64(ea)
                        if x is not None and x == x:
                            x = float(struct.unpack('>f', struct.pack('>f', x))[0]) if abs(x) < 3e38 else x
                            f.floats.append(x); f.events.append(('f', x, a))
                    else:
                        f.globals.append((ea, size, op, a))
                elif ra not in (0, 1):
                    dd = sext16(imm)
                    if 4 <= dd < 0x4000:
                        f.offsets.append(dd)
                        f.events.append(('o', dd, size, op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55), a))
                if op < 48 and op not in (36, 37, 38, 39, 44, 45, 47): known[d] = None  # loads write rD
                if op & 1 and op not in (47,): known[ra] = None  # update forms
                continue
            # anything else writing a GPR forgets it
            if op in (7, 8, 12, 13, 20, 21, 23, 28, 29, 26, 27, 25): known[d if op in (7, 8, 12, 13) else ra] = None
            elif op == 31:
                known[d] = None; known[ra] = None
                known[13] = R13; known[2] = R2

    def address(self, f, v, size, at):
        s = self.cstr(v)
        if s is not None: f.strings.append(s); f.events.append(('s', s, at))
        elif self.region(v): f.globals.append((v, size, 'addr', at))
