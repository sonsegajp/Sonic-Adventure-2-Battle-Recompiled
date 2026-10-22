# Dev helper: how much of what a mod's source uses is matched (functions, variables, patch sites).
#   python tools/pcmatch/coverage.py <mod source dir>
import collections, glob, pickle, re, sys
src = sys.argv[1]
P = pickle.load(open('build/pcmatch_pc.pkl', 'rb'))
G = pickle.load(open('build/pcmatch_gc.pkl', 'rb'))
pf, gf = P['funcs'], G['funcs']
pairs, data = {}, {}
for l in open('build/pcmatch.tsv'):
    t = l.rstrip('\n').split('\t')
    if t[0] == 'func': pairs[int(t[1], 16)] = (t[2], t[3])
    elif t[0] == 'data': data[int(t[1], 16)] = (t[2], t[4])
modname = {int(k): v for k, v in G['modfile'].items()}

def func_of(a):
    if a in pf: return a
    best = None
    for f, d in pf.items():
        if f <= a < d['end'] and (best is None or f > best): best = f
    return best

uses = collections.defaultdict(set)
for p in glob.glob(src + '/**/*.cpp', recursive=True) + glob.glob(src + '/**/*.h', recursive=True):
    text = re.sub(r'//[^\n]*', '', open(p, encoding='utf-8', errors='replace').read())
    for kind, rx in (('call', r'(?:FunctionPointer|VoidFunc|ObjectFunc|ThiscallFunctionPointer|StdcallFunctionPointer|FastcallFunctionPointer)\s*\([^;]*?(0x[0-9A-Fa-f]{6,7})\s*\)'),
                     ('hook', r'FunctionHook<[^>]*>\s*\w+\s*\(\s*(0x[0-9A-Fa-f]{6,7})'),
                     ('jump', r'WriteJump\s*\(\s*\(?\s*(?:void\s*\*\s*\)\s*)?(0x[0-9A-Fa-f]{6,7})'),
                     ('callsite', r'WriteCall\s*\(\s*\(?\s*(?:void\s*\*\s*\)\s*)?(0x[0-9A-Fa-f]{6,7})'),
                     ('patch', r'WriteData(?:<[^>]*>)?\s*\(\s*\(?[^,]*?(0x[0-9A-Fa-f]{6,7})'),
                     ('data', r'(?:DataPointer|DataArray)\s*\([^;]*?(0x[0-9A-Fa-f]{6,7})')):
        for m in re.finditer(rx, text): uses[kind].add(int(m.group(1), 16))
for kind in ('call', 'hook', 'jump', 'callsite', 'patch', 'data'):
    s = uses[kind]
    if kind == 'data':
        got = sum(1 for a in s if a in data)
    elif kind in ('call', 'hook'):
        got = sum(1 for a in s if a in pairs)
    else:
        got = sum(1 for a in s if (f := func_of(a)) is not None and f in pairs)
    print('%-9s %4d used, %4d matched (%s)' % (kind, len(s), got, 'the function containing it' if kind in ('jump', 'callsite', 'patch') else 'exactly'))
for a in sorted(uses['hook'])[:12]:
    print('  hook %06X -> %s' % (a, pairs.get(a, ('-', ''))))
