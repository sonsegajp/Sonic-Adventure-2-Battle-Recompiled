# Dev helper: summarise main.dol and every REL (header, sections, imports, relocation types).
import struct, glob, os, collections
def be(b, o, fmt='>I'): return struct.unpack_from(fmt, b, o)[0]
dol = open('orig/main.dol', 'rb').read()
offs = struct.unpack_from('>18I', dol, 0); addrs = struct.unpack_from('>18I', dol, 0x48); sizes = struct.unpack_from('>18I', dol, 0x90)
bss_a, bss_s, entry = struct.unpack_from('>3I', dol, 0xD8)
print('DOL entry %08x bss %08x+%x' % (entry, bss_a, bss_s))
for i in range(18):
    if sizes[i]: print('  %s%d %08x-%08x (%x)' % ('T' if i < 7 else 'D', i if i < 7 else i-7, addrs[i], addrs[i]+sizes[i], sizes[i]))
strs = {}
for p in glob.glob('orig/rel/*.str'): strs[os.path.basename(p)] = open(p,'rb').read()
RT = collections.Counter(); ids = {}
tot_text = 0
for p in sorted(glob.glob('orig/rel/*.rel')):
    b = open(p, 'rb').read()
    mid, nsec, secoff, nameoff, namesz, ver, bss = struct.unpack_from('>I8xIIIIII', b, 0)
    reloff, impoff, impsz = struct.unpack_from('>III', b, 0x24)
    psec, esec, usec, bsec = b[0x30:0x34]; prolog, epilog, unres = struct.unpack_from('>III', b, 0x34)
    secs = [struct.unpack_from('>II', b, secoff + 8*i) for i in range(nsec)]
    text = sum(sz for o, sz in secs if o & 1)
    tot_text += text
    imps = [struct.unpack_from('>II', b, impoff + 8*i) for i in range(impsz // 8)]
    nrel = 0
    for imod, ioff in imps:
        o = ioff
        while True:
            off, t, sec, add = struct.unpack_from('>HBBI', b, o); o += 8; RT[t] += 1; nrel += 1
            if t == 203: break
    name = ''
    for s in strs.values():
        if nameoff + namesz <= len(s) and namesz: name = s[nameoff:nameoff+namesz].split(b'\0')[0].decode('latin1')
    ids[mid] = os.path.basename(p)
    print('%-22s id=%3d v%d nsec=%2d text=%7x bss=%6x imports=%s relocs=%d prolog=%d:%x name=%r' % (
        os.path.basename(p), mid, ver, nsec, text, bss, [m for m, _ in imps], nrel, psec, prolog, name))
print('total REL text %x (%d KB)' % (tot_text, tot_text // 1024))
print('reloc types', dict(RT))
