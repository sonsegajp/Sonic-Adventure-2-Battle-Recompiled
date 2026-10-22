# Dev helper: shows matched pairs whose callees disagree, with both sides' features.
import collections, pickle, sys
P = pickle.load(open('build/pcmatch_pc.pkl', 'rb'))
G = pickle.load(open('build/pcmatch_gc.pkl', 'rb'))
pf, gf = P['funcs'], G['funcs']
pairs, why = {}, {}
for l in open('build/pcmatch.tsv'):
    t = l.rstrip('\n').split('\t')
    if t[0] == 'func':
        pairs[int(t[1], 16)] = [int(x, 16) for x in t[2].split(',')]
        why[int(t[1], 16)] = t[3]
rev = {}
for p, gs in pairs.items():
    for g in gs: rev[g] = p
n = 0
for p, gs in sorted(pairs.items()):
    g = gs[0]
    pcs = [t for t in pf[p]['calls'] if t in pairs]
    gcs = set(gf[g]['calls'])
    bad = [t for t in pcs if not (set(pairs[t]) & gcs)]
    if len(pcs) >= 3 and len(bad) * 2 > len(pcs) and why[p].startswith('features'):
        n += 1
        if n > int(sys.argv[1] if len(sys.argv) > 1 else 6): break
        print('=== PC %08X <-> GC %08X  %s  insns %d / %d' % (p, g, why[p], pf[p]['insns'], gf[g]['insns']))
        print('  PC strings', pf[p]['strings'][:4], 'floats', sorted(set(pf[p]['floats']))[:10])
        print('  GC strings', gf[g]['strings'][:4], 'floats', sorted(set(gf[g]['floats']))[:10])
        print('  PC offsets', sorted(collections.Counter(pf[p]['offsets']).items())[:14])
        print('  GC offsets', sorted(collections.Counter(gf[g]['offsets']).items())[:14])
        print('  PC calls ->', ['%08X->%s' % (t, ','.join('%08X' % x for x in pairs[t][:1])) for t in pcs[:8]])
        print('  GC calls  ', ['%08X%s' % (t, '(P%08X)' % rev[t] if t in rev else '') for t in gf[g]['calls'][:10]])
