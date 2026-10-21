# Dev helper: how much REL code is duplicated across modules, and do the copies sit at the same
# section offsets? Relocated instruction fields are masked so load addresses don't matter.
import struct, glob, os, collections, hashlib
def load(p):
    b = bytearray(open(p, 'rb').read())
    mid, nsec, secoff = struct.unpack_from('>I8xII', b, 0)
    impoff, impsz = struct.unpack_from('>II', b, 0x28)
    secs = [struct.unpack_from('>II', b, secoff + 8*i) for i in range(nsec)]
    relinfo = {}  # (sec, off) -> (type, tmod, tsec, addend)
    for i in range(impsz // 8):
        imod, ioff = struct.unpack_from('>II', b, impoff + 8*i)
        o = ioff; sec = 0; pos = 0
        while True:
            d, t, s, a = struct.unpack_from('>HBBI', b, o); o += 8
            if t == 203: break
            if t == 202: sec = s; pos = 0; continue
            pos += d
            if t == 201: continue
            relinfo[(sec, pos)] = (t, imod, s, a)
    code = {}
    for si, (o, sz) in enumerate(secs):
        if not (o & 1) or not sz: continue
        o &= ~1; words = []
        for k in range(0, sz, 4):
            w = struct.unpack_from('>I', b, o + k)[0]
            r = relinfo.get((si, k)) or relinfo.get((si, k + 2))
            if r:
                t = r[0]
                if t in (4, 5, 6): w &= 0xFFFF0000
                elif t == 10: w &= 0xFC000003
            words.append(w)
        code[si] = words
    return mid, code, relinfo
mods = {os.path.basename(p): load(p) for p in sorted(glob.glob('orig/rel/*.rel'))}
# 64-byte windows (16 masked words) at 4-byte steps
win = collections.defaultdict(set); total = 0
for name, (mid, code, _) in mods.items():
    for si, ws in code.items():
        for k in range(0, len(ws) - 16, 4):
            h = hashlib.blake2b(struct.pack('>16I', *ws[k:k+16]), digest_size=8).digest()
            win[h].add((name, si, k*4))
        total += len(ws)
uniq_words = 0; shared_same_off = 0; shared = 0; n = 0
for h, locs in win.items():
    n += 1
    if len(locs) > 1:
        shared += 1
        offs = {(si, off) for _, si, off in locs}
        if len(offs) == 1: shared_same_off += 1
print('REL text words', total, '| 64B windows', n, '| windows seen in >1 module', shared,
      '| of those at the SAME section offset everywhere', shared_same_off)
# per stage: what fraction of its windows also appear in stg03D?
ref = 'stg03D.rel'
refset = set()
for h, locs in win.items():
    if any(l[0] == ref for l in locs): refset.add(h)
for name in ['stg04D.rel', 'stg13D.rel', 'boss_bigbogyD.rel', 'advertiseD.rel', 'eventD.rel', 'titleD.rel']:
    mine = [h for h, locs in win.items() if any(l[0] == name for l in locs)]
    both = sum(1 for h in mine if h in refset)
    print('%-20s windows %7d  shared with %s: %5.1f%%' % (name, len(mine), ref, 100.0 * both / max(1, len(mine))))
