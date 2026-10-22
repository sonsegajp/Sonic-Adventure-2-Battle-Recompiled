# Dev helper: an independent check of matched pairs. Ordered struct-field accesses (offset, size,
# load/store) of both sides, compared for pairs where both have enough of them; pairs found by
# other evidence should mostly look alike here too.
import collections, pickle, sys
P = pickle.load(open('build/pcmatch_pc.pkl', 'rb'))
G = pickle.load(open('build/pcmatch_gc.pkl', 'rb'))
pf, gf = P['funcs'], G['funcs']
def acc(f): return [(e[1], e[2], e[3]) for e in sorted((e for e in f['events'] if e[0] == 'o'), key=lambda e: e[-1])][:400]
def sim(a, b):
    n, m = len(a), len(b)
    prev = [0] * (m + 1)
    for i in range(n):
        cur = [0] * (m + 1)
        for j in range(m):
            cur[j + 1] = prev[j] + 1 if a[i] == b[j] else max(prev[j + 1], cur[j])
        prev = cur
    return 2.0 * prev[m] / (n + m)
by = collections.defaultdict(list)
bad = collections.defaultdict(list)
for l in open('build/pcmatch.tsv'):
    t = l.rstrip('\n').split('\t')
    if t[0] != 'func': continue
    p, g = int(t[1], 16), int(t[2].split(',')[0], 16)
    a, b = acc(pf[p]), acc(gf[g])
    if len(a) < 10 or len(b) < 10: continue
    method = t[3].split()[0]
    s = sim(a, b)
    by[method].append(s)
    if s < 0.3: bad[method].append((s, p, g))
for m, v in sorted(by.items()):
    v.sort()
    low = sum(1 for x in v if x < 0.3)
    print('%-9s %5d checked  median %.2f  below 0.3: %d (%.0f%%)' % (m, len(v), v[len(v) // 2], low, 100.0 * low / len(v)))
if len(sys.argv) > 1:
    for s, p, g in sorted(bad[sys.argv[1]])[:15]: print('  %.2f  PC %08X  GC %08X' % (s, p, g))
