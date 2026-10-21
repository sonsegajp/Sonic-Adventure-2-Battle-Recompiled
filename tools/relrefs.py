# Finds code in a REL that references a file offset (through HA/LO relocations), and lists the
# functions containing those references (from build/funcs.tsv). Usage: relrefs.py <rel> <fileoff hex>
import struct, sys
path, foff = sys.argv[1], int(sys.argv[2], 16)
b = open(path, 'rb').read()
mid, nsec, secoff = struct.unpack_from('>I8xII', b, 0)
impoff, impsz = struct.unpack_from('>II', b, 0x28)
secs = [struct.unpack_from('>II', b, secoff + 8 * i) for i in range(nsec)]
tsec = toff = None
for i, (o, sz) in enumerate(secs):
    o &= ~1
    if o and o <= foff < o + sz: tsec, toff = i, foff - o
print('module', mid, 'target: section', tsec, 'offset', hex(toff))
refs = []
for i in range(impsz // 8):
    imod, ioff = struct.unpack_from('>II', b, impoff + 8 * i)
    o = ioff; sec = 0; pos = 0
    while True:
        d, t, s, a = struct.unpack_from('>HBBI', b, o); o += 8
        if t == 203: break
        if t == 202: sec = s; pos = 0; continue
        pos += d
        if imod == mid and s == tsec and t in (4, 6, 1) and abs(a - toff) < 0x40:
            refs.append((sec, pos, t, a))
funcs = []
for l in open('build/funcs.tsv'):
    m, s, e, al = l.split('\t')
    if int(m) == mid: funcs.append((int(s, 16), int(e, 16)))
for sec, pos, t, a in refs:
    addr = (sec << 24) | (pos & ~3)
    f = [s for s, e in funcs if s <= addr < e]
    print('  ref sec %d off %x type %d addend %x in function %s' % (sec, pos, t, a, ['m%d_%08X' % (mid, x) for x in f]))
