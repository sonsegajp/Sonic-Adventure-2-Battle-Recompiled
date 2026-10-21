# Dev helper: pull main.dol, every .rel and the .str module-name tables out of the user's own disc
# into orig/ (gitignored). The shipped program reads the disc itself (src/disc).
import struct, sys, os
iso = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else 'orig'
f = open(iso, 'rb')
def rd(off, n): f.seek(off); return f.read(n)
dol_off, fst_off, fst_size = struct.unpack('>III', rd(0x420, 12))
fst = rd(fst_off, fst_size)
n = struct.unpack('>I', fst[8:12])[0]
strtab = fst[n*12:]
def nm(o): return strtab[o:strtab.index(b'\0', o)].decode('latin1')
files = []
def walk(i, end, path):
    while i < end:
        a, off, ln = struct.unpack('>III', fst[i*12:i*12+12])
        name = nm(a & 0xFFFFFF)
        if a >> 24: walk(i+1, ln, path+name+'/'); i = ln
        else: files.append((path+name, off, ln)); i += 1
walk(1, n, '')
os.makedirs(out + '/rel', exist_ok=True)
# DOL size = max(section offset + size) over the 18 sections
hdr = rd(dol_off, 0x100)
offs = struct.unpack('>18I', hdr[0:72]); sizes = struct.unpack('>18I', hdr[0x90:0xD8])
dol_size = max(o + s for o, s in zip(offs, sizes) if s)
open(out + '/main.dol', 'wb').write(rd(dol_off, dol_size))
cnt = 0
for p, off, ln in files:
    if p.lower().endswith(('.rel', '.str')):
        open(out + '/rel/' + p.replace('/', '_'), 'wb').write(rd(off, ln)); cnt += 1
with open(out + '/files.tsv', 'w') as m:
    for p, off, ln in files: m.write('%s\t%x\t%d\n' % (p, off, ln))
print('main.dol', dol_size, 'bytes;', cnt, 'rel/str files;', len(files), 'files listed')
