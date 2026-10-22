# Dev helper: what a mod's patches (build/modhost_patches_<mod>.tsv, SA2_MOD_DUMP=1) need from the
# map, by kind, and whether pcmap_auto/pcmap cover them.
#   python tools/pcmatch/modneeds.py "<mod name>"
import bisect, collections, pickle, struct, sys
import pefile
mod = sys.argv[1]
P = pickle.load(open('build/pcmatch_pc.pkl', 'rb'))
pf, secs = P['funcs'], P['secs']
img = pefile.PE(r'C:/Users/hyper/AppData/Local/Temp/claude/C--Users-hyper/e3397157-2ca6-41da-b984-8b057bbec202/scratchpad/mods/pc/sonic2app.exe.unpacked.exe', fast_load=True).get_memory_mapped_image()
def pcb(a, n): return img[a - 0x400000:a - 0x400000 + n]
funcs, insns, data = {}, {}, []
for path in ('config/GSNE8P/pcmap_auto.txt', 'config/GSNE8P/pcmap.txt'):
    for l in open(path):
        t = l.split()
        if not t or t[0].startswith('#'): continue
        if t[0] == 'func': funcs[int(t[1], 16)] = t[2]
        elif t[0] == 'insn': insns[int(t[1], 16)] = t[3]
        elif t[0] == 'data': data.append((int(t[1], 16), int(t[3], 0)))
data.sort()
dstarts = [a for a, _ in data]
def in_data(a):
    i = bisect.bisect_right(dstarts, a) - 1
    return i >= 0 and a < data[i][0] + data[i][1]
starts = sorted(pf)
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 and a < pf[starts[i]]['end'] else None
def sec(a):
    for n, (s, e) in secs.items():
        if s <= a < e: return n
    return '?'
kinds = collections.Counter(); cover = collections.Counter(); examples = collections.defaultdict(list)
for l in open('build/modhost_patches_%s.tsv' % mod):
    a, hx = l.split()
    a = int(a, 16); b = bytes.fromhex(hx)
    s = sec(a)
    if s == '.text':
        f = func_of(a)
        orig = pcb(a, len(b))
        if f == a and b[0] == 0xE9: k = 'function replaced (jmp at its start)'; ok = a in funcs
        elif b[0] == 0xE8 and orig[0] == 0xE8: k = 'call redirected'; ok = a in insns
        elif b[0] == 0xE9: k = 'jump written inside a function'; ok = a in insns
        elif b[0] == 0xE8: k = 'call written over other code'; ok = a in insns
        else: k = 'bytes changed inside a function'; ok = any(x in insns for x in range(a - 15, a + 1))
        k += '' if f in funcs else ' [function unmatched]' if f else ' [no PC function]'
    else:
        v = struct.unpack('<I', b[:4])[0] if len(b) >= 4 else None
        what = 'pointer to mod code/data' if v and 0x10000000 <= v < 0x70000000 else 'value'
        k = 'data in %s: %s' % (s, what); ok = in_data(a)
    kinds[k] += 1; cover[k] += ok
    if len(examples[k]) < 4 and not ok: examples[k].append('%06X' % a)
for k, n in kinds.most_common():
    print('%4d  %-62s translatable now: %d   not yet e.g. %s' % (n, k, cover[k], ' '.join(examples[k])))
