# Analyses both builds once and caches the results (build/pcmatch_*.pkl) for the matcher.
#   python tools/pcmatch/analyze.py <unpacked sonic2app.exe>
import os, pickle, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import pc, gcside, seeds

FIELDS = ('insns', 'calls', 'callsites', 'strings', 'floats', 'offsets', 'consts', 'globals', 'end', 'events')

def dump(funcs, extra):
    out = {}
    for a, f in funcs.items():
        d = {k: getattr(f, k) for k in FIELDS}
        for k in extra: d[k] = getattr(f, k)
        out[a] = d
    return out

def main():
    exe = sys.argv[1]
    t = time.time()
    sf, sd = seeds.scan(seeds.default_paths())
    p = pc.PcExe(exe)
    pf = p.analyze(list(sf.keys()))
    print('PC: %d functions (%.0f s)' % (len(pf), time.time() - t))
    pickle.dump({'funcs': dump(pf, ()), 'seeds_f': sf, 'seeds_d': sd, 'secs': p.secs}, open('build/pcmatch_pc.pkl', 'wb'))
    t = time.time()
    g = gcside.GcGame()
    gf = g.load_funcs()
    print('GC: %d functions (%.0f s)' % (len(gf), time.time() - t))
    regions = [(r.start, r.end, r.name, r.module, r.section, r.code) for r in g.regions]
    data = [(r.start, bytes(r.data)) for r in g.regions if not r.code and r.name != 'bss']
    pickle.dump({'funcs': dump(gf, ('module', 'rel', 'hash')), 'regions': regions, 'modbase': g.modbase, 'modfile': g.modfile,
                 'data': data}, open('build/pcmatch_gc.pkl', 'wb'))

if __name__ == '__main__':
    main()
