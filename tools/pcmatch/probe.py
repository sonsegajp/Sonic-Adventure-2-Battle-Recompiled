# Dev helper: for given PC functions, the best GameCube candidates by every signal.
#   python tools/pcmatch/probe.py <pc addr> [...]   (or "cwe": the functions CWE replaces, unmatched)
import collections, math, pickle, struct, sys
P = pickle.load(open('build/pcmatch_pc.pkl', 'rb'))
G = pickle.load(open('build/pcmatch_gc.pkl', 'rb'))
pf, gf = P['funcs'], G['funcs']
pairs = {}
for l in open('build/pcmatch.tsv'):
    t = l.rstrip('\n').split('\t')
    if t[0] == 'func': pairs[int(t[1], 16)] = int(t[2].split(',')[0], 16)
taken = set(pairs.values())
mods = {int(k): v for k, v in G['modfile'].items()}
def acc(f): return [(e[1], e[2], e[3]) for e in sorted((e for e in f['events'] if e[0] == 'o'), key=lambda e: e[-1])][:300]
def lcs(a, b):
    n, m = len(a), len(b)
    if not n or not m: return 0.0
    prev = [0] * (m + 1)
    for i in range(n):
        cur = [0] * (m + 1)
        for j in range(m):
            cur[j + 1] = prev[j] + 1 if a[i] == b[j] else max(prev[j + 1], cur[j])
        prev = cur
    return 2.0 * prev[m] / (n + m)
def fl(f): return set(round(x, 4) for x in f['floats'] if x not in (0.0, 1.0) and abs(x) < 1e9)
def describe(f): return '%d insns, %d accesses, floats %s, %d calls, strings %s' % (
    f['insns'], len(acc(f)), sorted(fl(f))[:6], len(set(f['calls'])), f['strings'][:2])
targets = []
if sys.argv[1] == 'cwe':
    for l in open('build/modhost_patches_Chao World Extended.tsv'):
        a = int(l.split()[0], 16)
        if a in pf and a not in pairs and l.split()[1].startswith('E9'): targets.append(a)
else:
    targets = [int(x, 16) for x in sys.argv[1:]]
chao = [g for g, f in gf.items() if 60 <= f['module'] <= 79 and g not in taken]
for x in targets[:int(sys.argv[2]) if sys.argv[1] == 'cwe' and len(sys.argv) > 2 else 12]:
    f = pf[x]
    print('PC %06X: %s' % (x, describe(f)))
    ax, fx = acc(f), fl(f)
    sc = []
    for g in chao:
        h = gf[g]
        r = (h['insns'] + 4) / (f['insns'] + 4)
        if not 0.4 <= r <= 2.6: continue
        fs = len(fx & fl(h)) / max(1, len(fx | fl(h))) if (fx or fl(h)) else 0
        s = lcs(ax, acc(h)) if len(ax) * len(acc(h)) < 40000 else 0
        sc.append((s + fs, s, fs, g))
    sc.sort(reverse=True)
    for tot, s, fs, g in sc[:4]:
        print('    GC %08X %-26s acc %.2f floats %.2f  %s' % (g, mods.get(gf[g]['module'], 'dol'), s, fs, describe(gf[g])))
