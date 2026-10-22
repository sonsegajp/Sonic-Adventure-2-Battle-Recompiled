# Matches PC functions and variables to the GameCube's (from analyze.py's caches).
# 1. Anchors: pairs with strong evidence that survives recompilation - a string only these two
#    functions use, or several rare float constants and struct offsets in common.
# 2. Growth through the call graph: an unmatched function whose matched callers and callees
#    correspond to one GameCube function's (and whose features do not contradict it) joins.
#    Rounds repeat while pairs are added; ambiguous candidates wait for more evidence.
# 3. Variables: the globals matched function pairs touch, aligned in order and voted on.
# Writes build/pcmatch.tsv.
import collections, math, pickle

P = pickle.load(open('build/pcmatch_pc.pkl', 'rb'))
G = pickle.load(open('build/pcmatch_gc.pkl', 'rb'))
pf, gf = P['funcs'], G['funcs']
WEIGHT = {'s': 4.0, 'f': 1.6, 'o': 1.0, 'c': 0.6}

def tokens(f):
    t = collections.Counter()
    for s in f['strings']: t['s:' + s] += 1
    for x in f['floats']:
        if x not in (0.0, 1.0): t['f:%.7g' % x] += 1
    for o in f['offsets']: t['o:%d' % o] += 1
    for c in f['consts']:
        if c > 2: t['c:%d' % c] += 1
    return t

# GameCube functions with identical code features (copies in several RELs) form one class.
classes = collections.defaultdict(list)
for a, f in gf.items():
    # the same code: copies of one function (stage modules each carry common code); tiny functions
    # (stubs) look alike without being the same, so they stand alone
    classes[(f['insns'], f['hash']) if f['insns'] >= 8 else ('addr', a)].append(a)
gclass, members = {}, {}
for ms in classes.values():
    r = min(ms)
    members[r] = sorted(ms)
    for m in ms: gclass[m] = r
def rep(ga): return gclass.get(ga, ga)

# ---- data contents (function pointer tables) ----
import bisect, struct, pefile
PC_EXE = r'C:/Users/hyper/AppData/Local/Temp/claude/C--Users-hyper/e3397157-2ca6-41da-b984-8b057bbec202/scratchpad/mods/pc/sonic2app.exe.unpacked.exe'
_pe = pefile.PE(PC_EXE, fast_load=True)
_pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
PCIMG = _pe.get_memory_mapped_image()
PCRELOC = set()
for blk in getattr(_pe, 'DIRECTORY_ENTRY_BASERELOC', []):
    for e in blk.entries:
        if e.type == 3: PCRELOC.add(0x400000 + e.rva)
def pc_u32(a):
    o = a - 0x400000
    return struct.unpack_from('<I', PCIMG, o)[0] if 0 <= o < len(PCIMG) - 3 else None
GDATA = sorted(G['data'])
GSTARTS = [s0 for s0, _ in GDATA]
def gc_u32(a):
    i = bisect.bisect_right(GSTARTS, a) - 1
    if i < 0: return None
    s0, d = GDATA[i]
    o = a - s0
    return struct.unpack_from('>I', d, o)[0] if 0 <= o < len(d) - 3 else None

pt = {a: tokens(f) for a, f in pf.items()}
gt = {a: tokens(gf[a]) for a in members}
dfP, dfG = collections.Counter(), collections.Counter()
for t in pt.values(): dfP.update(t.keys())
for t in gt.values(): dfG.update(t.keys())
NP, NG = len(pt), len(gt)
def weight(tok): return WEIGHT[tok[0]] * math.log(1 + (NP + NG) / (dfP[tok] + dfG[tok]))
def norm(t): return math.sqrt(sum(weight(k) * v * v for k, v in t.items())) or 1.0
pn = {a: norm(t) for a, t in pt.items()}
gn = {a: norm(t) for a, t in gt.items()}

def score(pa, ga):
    tp, tg = pt[pa], gt[ga]
    s = sum(weight(k) * min(v, tg[k]) ** 2 for k, v in tp.items() if k in tg)
    return s / (pn[pa] * gn[ga])

# Ordered struct-field accesses (offset, size, store): the same fields in the same order in both builds.
def accesses(f):
    return [(e[1], e[2], e[3]) for e in sorted((e for e in f['events'] if e[0] == 'o'), key=lambda e: e[-1])][:500]
pacc = {a: accesses(f) for a, f in pf.items()}
gacc = {a: accesses(gf[a]) for a in members}
_seq_cache = {}
def seqsim(pa, ga):
    k = (pa, ga)
    if k in _seq_cache: return _seq_cache[k]
    a, b = pacc[pa], gacc[ga]
    if len(a) < 4 or len(b) < 4:
        _seq_cache[k] = None
        return None
    # LCS on (offset, size); the store bit must agree too
    n, m = len(a), len(b)
    prev = [0] * (m + 1)
    for i in range(n):
        cur = [0] * (m + 1)
        ai = a[i]
        for j in range(m):
            cur[j + 1] = prev[j] + 1 if ai == b[j] else (prev[j + 1] if prev[j + 1] > cur[j] else cur[j])
        prev = cur
    r = 2.0 * prev[m] / (n + m)
    _seq_cache[k] = r
    return r

# Evidence against a pair: one side's rare strings and float constants mostly missing on the other
# (two recursive tree walkers in alike places of the call graph are not the same function if one
# has the Chao tree's angles and the other none). Only what both builds have counts: the PC's
# assert messages and file names are compiled out of the GameCube build.
def rare_sf(t):
    return {k for k in t if k[0] in 'sf' and dfP[k] and dfG[k] and dfP[k] + dfG[k] <= 12}
def contradicts(pa, ga, strong=False):
    # field accesses in a different order: not the same function (unless the call graph says so
    # firmly - `strong`: the GameCube compiler schedules loads and stores differently)
    a, b = pacc[pa], gacc[ga]
    if not strong and len(a) >= 10 and len(b) >= 10 and len(a) * len(b) <= 160000:
        r = seqsim(pa, ga)
        if r is not None and r < 0.3: return True
    rp, rg = rare_sf(pt[pa]), rare_sf(gt[ga])
    both = rp & rg
    if len(rp) >= 2 and len(both) * 3 < len(rp): return True
    if len(rg) >= 2 and len(both) * 3 < len(rg): return True
    if (len(rp) >= 3 and not rg) or (len(rg) >= 3 and not rp): return True
    return False

def ratio_ok(pa, ga, lo=0.35, hi=3.5):
    return lo <= (gf[ga]['insns'] + 4) / (pf[pa]['insns'] + 4) <= hi

# call graphs (GameCube by class)
pcallees = {a: [t for t in f['calls'] if t in pf] for a, f in pf.items()}
gcallees = {a: [rep(t) for t in gf[a]['calls'] if rep(t) in gt] for a in gt}
pcallers, gcallers = collections.defaultdict(set), collections.defaultdict(set)
for a, cs in pcallees.items():
    for t in cs: pcallers[t].add(a)
for a, cs in gcallees.items():
    for t in cs: gcallers[t].add(a)

pairs, why = {}, {}
taken = {}
def add(pa, ga, reason):
    pairs[pa] = ga; taken[ga] = pa; why[pa] = reason

# ---- 1. anchors ----
post = collections.defaultdict(list)
for a, t in gt.items():
    for k, v in t.items():
        if dfG[k] <= 200: post[k].append(a)
cands = {}
for pa, t in pt.items():
    if pn[pa] < 4: continue
    acc = collections.Counter()
    for k, v in t.items():
        for ga in post.get(k, ()): acc[ga] += weight(k)
    sc = sorted(((score(pa, ga), ga) for ga, _ in acc.most_common(30)), reverse=True)
    cands[pa] = sc[:6]
best_g = collections.defaultdict(list)
for pa, sc in cands.items():
    if sc: best_g[sc[0][1]].append((sc[0][0], pa))
for pa, sc in cands.items():
    if not sc: continue
    s0, g0 = sc[0]
    s1 = sc[1][0] if len(sc) > 1 else 0
    tp, tg = pt[pa], gt[g0]
    shared = [k for k in tp if k in tg]
    unique_str = any(k[0] == 's' and dfP[k] == 1 and dfG[k] == 1 for k in shared)
    rare = sum(1 for k in shared if dfP[k] + dfG[k] <= 6 and k[0] in 'sf')
    strong = (unique_str and s0 >= 0.35) or (rare >= 3 and s0 >= 0.6) or s0 >= 0.85
    if not strong or s1 > 0.75 * s0 or not ratio_ok(pa, g0, 0.3, 4.0): continue
    rivals = sorted(best_g[g0], reverse=True)
    if rivals[0][1] != pa or (len(rivals) > 1 and rivals[1][0] > 0.8 * s0): continue
    add(pa, g0, 'anchor %.2f' % s0)
n_acc = 0
for pa, sc in cands.items():
    if pa in pairs or len(pacc[pa]) < 25: continue
    best = []
    for s0, g0 in sc:
        if g0 in taken or len(gacc[g0]) < 20 or len(pacc[pa]) * len(gacc[g0]) > 250000: continue
        r = seqsim(pa, g0)
        if r is not None: best.append((r, g0))
    best.sort(reverse=True)
    if best and best[0][0] >= 0.6 and (len(best) == 1 or best[1][0] < 0.75 * best[0][0]):
        add(pa, best[0][1], 'accesses %.2f' % best[0][0]); n_acc += 1
print('anchors: %d (%d by field accesses)' % (len(pairs), n_acc))

# ---- 1c. data contents ----
# Numeric tables compile to the same values in both builds (byte order apart; pointers differ):
# data blocks the functions reference are matched by content, and functions referencing a matched
# block on each side - and no other function referencing it - pair up.
GREG = [(a0, a1) for a0, a1, name, mod, sec, code in G['regions']]
def gc_is_ptr(v):
    return any(a0 <= v < a1 for a0, a1 in GREG)
def pc_words(a, n=16):
    out = []
    for k in range(n):
        if a + 4 * k in PCRELOC: out.append(None)
        else:
            v = pc_u32(a + 4 * k)
            if v is None: break
            out.append(v)
    return out
def gc_words(a, n=16):
    out = []
    for k in range(n):
        v = gc_u32(a + 4 * k)
        if v is None: break
        out.append(None if gc_is_ptr(v) else v)
    return out
def swap16(v): return ((v & 0xFFFF) << 16) | (v >> 16)  # u16 pairs: BE word as two LE halves
COMMON = {0, 1, 0x3F800000, 0x3F000000, 0xBF800000, 0xFFFFFFFF, 0x40000000}
def content_key(ws):
    """12 words, pointers' places included: distinctive enough to name a block."""
    ws = list(ws[:12])
    if len(ws) < 12: return None
    vals = [w for w in ws if w is not None]
    if len(vals) < 6 or len(set(vals)) < 3 or sum(1 for w in vals if w not in COMMON) < 2: return None
    return tuple(ws)

pref = collections.defaultdict(set)  # PC data address -> functions referencing it
gref = collections.defaultdict(set)
for a, f in pf.items():
    for v, sz, m, at in f['globals']: pref[v].add(a)
for a in gt:
    for v, sz, m, at in gf[a]['globals']: gref[v].add(a)
gindex = collections.defaultdict(set)
for v in gref:
    ws = gc_words(v)
    for conv in (lambda w: w, swap16):
        k = content_key([conv(w) if w is not None else None for w in ws])
        if k: gindex[k].add(v)
pindex = collections.defaultdict(set)
for v in pref:
    if 0x873000 <= v < 0x2670000:  # .rdata / .data
        k = content_key(pc_words(v))
        if k: pindex[k].add(v)
data_anchor = 0
dpairs = {}
for k, vs in pindex.items():
    gs = gindex.get(k)
    if len(vs) != 1 or not gs or len(gs) != 1: continue  # the same content once on each side
    v, g = next(iter(vs)), next(iter(gs))
    dpairs[v] = g
    xs, ys = pref[v], {rep(y) for y in gref[g]}
    if len(xs) == 1 and len(ys) == 1:
        x, y = next(iter(xs)), next(iter(ys))
        if x not in pairs and y not in taken and ratio_ok(x, y, 0.3, 4.0) and not contradicts(x, y):
            add(x, y, 'data'); data_anchor += 1
print('data blocks matched by content: %d; functions anchored by them: %d' % (len(dpairs), data_anchor))

# ---- 1b. hub functions by their call sites' arguments ----
# The same strings and constants are passed at the same calls in both builds (file names, format
# strings, sound ids): a callee pair gets a vote per rare argument both sides pass it.
def callsite_args(f):
    out, cur = [], []
    for e in sorted(f['events'], key=lambda e: e[-1]):
        if e[0] == 'call':
            out.append((e[1], cur)); cur = []
        elif e[0] == 's' or (e[0] == 'c' and e[1] >= 0x100) or (e[0] == 'f' and e[1] not in (0.0, 1.0)):
            cur.append('%s:%r' % (e[0], e[1]))
    return out
pargs = collections.defaultdict(list)  # token -> PC callees
gargs = collections.defaultdict(list)
for a, f in pf.items():
    for callee, toks in callsite_args(f):
        if callee in pf:
            for t in set(toks[-4:]): pargs[t].append(callee)
for a in gt:
    for callee, toks in callsite_args(gf[a]):
        callee = rep(callee)
        if callee in gt:
            for t in set(toks[-4:]): gargs[t].append(callee)
avotes = collections.defaultdict(collections.Counter)
for t, xs in pargs.items():
    ys = gargs.get(t)
    if not ys or len(set(xs)) > 3 or len(set(ys)) > 3 or len(xs) > 40: continue
    for x in set(xs):
        for y in set(ys): avotes[x][y] += 1
by_y = collections.defaultdict(collections.Counter)
for x, c in avotes.items():
    for y, n in c.items(): by_y[y][x] += n
nhub = 0
for x, c in avotes.items():
    top = c.most_common(2)
    y, n = top[0]
    n2 = top[1][1] if len(top) > 1 else 0
    ytop = by_y[y].most_common(2)
    if x in pairs or y in taken or n < 3 or n < 2 * n2: continue
    if ytop[0][0] != x or (len(ytop) > 1 and ytop[1][1] * 2 > n): continue
    if not ratio_ok(x, y, 0.2, 6.0) or contradicts(x, y): continue
    add(x, y, 'args %d' % n); nhub += 1
print('hubs by call arguments: %d' % nhub)

# ---- 2. growth through the call graph ----
def lcs_align(a, b, eq):
    n, m = len(a), len(b)
    L = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            L[i][j] = L[i + 1][j + 1] + 1 if eq(a[i], b[j]) else max(L[i + 1][j], L[i][j + 1])
    out, i, j = [], 0, 0
    while i < n and j < m:
        if eq(a[i], b[j]): out.append((i, j)); i += 1; j += 1
        elif L[i + 1][j] >= L[i][j + 1]: i += 1
        else: j += 1
    return out

def support(pa, ga):
    """Matched neighbours that agree: callers of pa matched to callers of ga, callees likewise."""
    s = 0
    for c in pcallers[pa]:
        if c in pairs and pairs[c] in gcallers[ga]: s += 1
    gcs = set(gcallees[ga])
    for c in set(pcallees[pa]):
        if c in pairs and pairs[c] in gcs: s += 1
    return s

def position_votes():
    """In matched pairs' call sequences, unmatched callees in the same position between matched ones."""
    v = collections.defaultdict(collections.Counter)
    for pa, ga in pairs.items():
        a, b = pcallees[pa], gcallees[ga]
        if not a or not b or len(a) * len(b) > 40000: continue
        anchors = lcs_align(a, b, lambda x, y: pairs.get(x) == y)
        bounds = [(-1, -1)] + anchors + [(len(a), len(b))]
        for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
            gp = [x for x in dict.fromkeys(a[i0 + 1:i1]) if x not in pairs]
            gg = [y for y in dict.fromkeys(b[j0 + 1:j1]) if y not in taken]
            if gp and len(gp) == len(gg):
                for x, y in zip(gp, gg): v[x][y] += 1
    return v

# Address order: both compilers emit a source file's functions in source order and both linkers
# keep object order, so between two matched functions close together on both sides, the unmatched
# functions in between pair up in order when their counts agree.
G_REGIONS = [(a, b) for a, b, name, mod, sec, code in G['regions'] if code]
def gregion(a):
    for i, (s0, e0) in enumerate(G_REGIONS):
        if s0 <= a < e0: return i
    return -1
porder = sorted(pf)
pindex = {a: i for i, a in enumerate(porder)}
gorder = sorted(gt)
gindex = {a: i for i, a in enumerate(gorder)}

def order_sim(x, y):
    """How alike two functions in matching places of the layout are (sizes, features, accesses)."""
    if contradicts(x, y): return -2.0
    r = (gf[y]['insns'] + 4) / (pf[x]['insns'] + 4)
    if not 0.4 <= r <= 2.8: return -1.0
    size = 1.0 - min(1.0, abs(math.log(r)))
    ev = 0.0
    if pn.get(x, 0) > 2 and gn.get(y, 0) > 2: ev = max(ev, score(x, y))
    if 4 <= len(pacc[x]) and 4 <= len(gacc[y]) and len(pacc[x]) * len(gacc[y]) < 60000:
        ev = max(ev, seqsim(x, y) or 0.0)
    return 0.6 * size + ev - 0.35

def order_proposals():
    """Between two matched pairs close together on both sides (the GameCube side either way), the
    unmatched functions in between are aligned (Needleman-Wunsch: functions one side inlined
    are gaps) and the clearly alike aligned ones pair."""
    out = []
    matched = sorted((pindex[pa], gindex[ga], pa, ga) for pa, ga in pairs.items())
    for (i0, j0, p0, g0), (i1, j1, p1, g1) in zip(matched, matched[1:]):
        if not (1 < i1 - i0 <= 48 and 1 < abs(j1 - j0) <= 48): continue
        if gregion(g0) != gregion(g1): continue
        xs = [porder[k] for k in range(i0 + 1, i1) if porder[k] not in pairs]
        ys = [gorder[k] for k in (range(j0 + 1, j1) if j1 > j0 else range(j0 - 1, j1, -1)) if gorder[k] not in taken]
        if not xs or not ys: continue
        n, m = len(xs), len(ys)
        gap = 0.15
        S = [[0.0] * (m + 1) for _ in range(n + 1)]
        B = [[0] * (m + 1) for _ in range(n + 1)]
        for i in range(1, n + 1): S[i][0] = -gap * i; B[i][0] = 1
        for j in range(1, m + 1): S[0][j] = -gap * j; B[0][j] = 2
        sims = {}
        for i in range(1, n + 1):
            for j in range(1, m + 1):
                sm = order_sim(xs[i - 1], ys[j - 1]); sims[(i, j)] = sm
                d, u, l = S[i - 1][j - 1] + sm, S[i - 1][j] - gap, S[i][j - 1] - gap
                if d >= u and d >= l: S[i][j], B[i][j] = d, 0
                elif u >= l: S[i][j], B[i][j] = u, 1
                else: S[i][j], B[i][j] = l, 2
        i, j = n, m
        while i > 0 and j > 0:
            if B[i][j] == 0:
                if sims[(i, j)] >= 0.2: out.append((xs[i - 1], ys[j - 1], sims[(i, j)]))
                i -= 1; j -= 1
            elif B[i][j] == 1: i -= 1
            else: j -= 1
    return out

def match_variables():
    votes = collections.defaultdict(collections.Counter)
    sizes = collections.defaultdict(collections.Counter)
    for pa, ga in pairs.items():
        pg = [(a, s0) for a, s0, m, at in pf[pa]['globals']]
        gg = [(a, s0) for a, s0, m, at in gf[ga]['globals']]
        if not pg or not gg or len(pg) * len(gg) > 40000: continue
        for i, j in lcs_align(pg, gg, lambda x, y: x[1] == y[1] or not x[1] or not y[1]):
            votes[pg[i][0]][gg[j][0]] += 1
            if pg[i][1] and gg[j][1]: sizes[pg[i][0]][gg[j][1]] += 1
    out = {}
    for a, v in votes.items():
        top = v.most_common(2)
        n0 = top[0][1]; n1 = top[1][1] if len(top) > 1 else 0
        if (n0 >= 2 and n0 >= 2 * n1) or (n0 == 1 and n1 == 0): out[a] = (top[0][0], n0, sum(v.values()))
    return out, sizes

def table_proposals(data):
    """Matched variables that are tables of function pointers (or of structs holding them): entries
    at the same offsets pair up."""
    votes = collections.defaultdict(collections.Counter)
    for pa, (ga, n, tot) in data.items():
        miss = 0
        for k in range(0, 0x800, 4):
            pv = pc_u32(pa + k) if (pa + k) in PCRELOC else None
            gv = gc_u32(ga + k)
            pfun = pv if pv in pf else None
            gfun = rep(gv) if gv is not None and rep(gv) in gt else None
            if pfun and gfun:
                votes[pfun][gfun] += 1; miss = 0
            elif pfun or gfun:
                miss += 1
                if miss > 2: break
            elif pv is None and gv is None:
                break
    out = []
    for x, c in votes.items():
        if x in pairs: continue
        top = c.most_common(2)
        y, n = top[0]
        if y in taken or (len(top) > 1 and top[1][1] >= n) or not ratio_ok(x, y, 0.3, 4.0) or contradicts(x, y): continue
        fs = score(x, y) if (pn.get(x, 0) > 2 and gn.get(y, 0) > 2) else None
        if fs is not None and fs < 0.1 and pn[x] > 6 and gn[y] > 6: continue
        out.append((x, y, n, fs or 0.0))
    return out

def window_anchors():
    """Unmatched functions with enough field accesses, compared with the unmatched functions of the
    GameCube modules their part of the PC executable corresponds to (by the pairs around them)."""
    gmod = {a: gf[a]['module'] for a in gt}
    win = collections.defaultdict(collections.Counter)
    for pa, ga in pairs.items(): win[pa >> 16][gmod[ga]] += 1
    by_mod = collections.defaultdict(list)
    for ga in gt:
        if ga not in taken and len(gacc[ga]) >= 10: by_mod[gmod[ga]].append(ga)
    offs = {}
    def offset_set(acc): return {o for o, sz, st in acc}
    out = []
    for x in pf:
        if x in pairs or len(pacc[x]) < 12: continue
        mods = collections.Counter()
        for w in (x >> 16) - 1, x >> 16, (x >> 16) + 1: mods.update(win.get(w, {}))
        if not mods: continue
        ox = offset_set(pacc[x])
        cands = []
        for m, _ in mods.most_common(3):
            for y in by_mod[m]:
                if y in taken or not ratio_ok(x, y, 0.45, 2.4): continue
                oy = offs.get(y)
                if oy is None: oy = offs[y] = offset_set(gacc[y])
                j = len(ox & oy) / max(1, len(ox | oy))
                if j >= 0.3: cands.append((j, y))
        cands.sort(reverse=True)
        sc = []
        for j, y in cands[:8]:
            if len(pacc[x]) * len(gacc[y]) > 160000: continue
            r = seqsim(x, y)
            if r is not None and not contradicts(x, y): sc.append((r, y))
        sc.sort(reverse=True)
        if sc and sc[0][0] >= 0.55 and (len(sc) == 1 or sc[1][0] < 0.85 * sc[0][0]):
            out.append((sc[0][0], x, sc[0][1]))
    return out

for rnd in range(60):
    norder = 0
    if rnd % 3 == 1:
        for r, x, y in sorted(window_anchors(), reverse=True):
            if x in pairs or y in taken: continue
            add(x, y, 'window %.2f' % r); norder += 1
    if rnd % 2 == 0:
        _data, _ = match_variables()
        for x, y, n, fs in table_proposals(_data):
            if x in pairs or y in taken: continue
            add(x, y, 'table %d f%.2f' % (n, fs)); norder += 1
    for x, y, fs in order_proposals():
        if x in pairs or y in taken: continue
        add(x, y, 'order f%.2f' % fs); norder += 1
    cand = collections.defaultdict(set)
    for x, c in position_votes().items():
        for y in c: cand[x].add(y)
    # callers' partners' callees, callees' partners' callers: of a function's matched neighbours,
    # the one with the fewest such GameCube functions gives the candidates (a hub callee alone -
    # thousands of callers - would drown them), the support below counts all of them
    seeds = {}
    for pa, ga in list(pairs.items()):
        ys = [y for y in set(gcallees[ga]) if y not in taken]
        for x in set(pcallees[pa]):
            if x not in pairs and (x not in seeds or len(ys) < len(seeds[x])): seeds[x] = ys
        ys = None
        for x in pcallers[pa]:
            if x not in pairs:
                if ys is None: ys = [y for y in gcallers[ga] if y not in taken]
                if x not in seeds or len(ys) < len(seeds[x]): seeds[x] = ys
    for x, ys in seeds.items():
        if len(ys) <= 400: cand[x].update(ys)
    added = 0
    proposals = []
    for x, ys in cand.items():
        if len(ys) > 400: continue
        scored = []
        for y in ys:
            if not ratio_ok(x, y, 0.45, 2.6): continue
            sup = support(x, y)
            if sup == 0 or contradicts(x, y, strong=sup >= 3): continue
            fs = score(x, y) if (pn.get(x, 0) > 2 and gn.get(y, 0) > 2) else None
            if fs is not None and fs < 0.1 and pn[x] > 6 and gn[y] > 6: continue  # features say no
            ss = seqsim(x, y) if len(pacc[x]) * len(gacc[y]) < 250000 else None
            if sup < 3 and ss is not None and ss < 0.25 and len(pacc[x]) > 12 and len(gacc[y]) > 12: continue  # accesses say no
            ev = max(fs or 0.0, ss or 0.0)
            scored.append((sup + 3 * ev, sup, ev, y))
        if not scored: continue
        scored.sort(reverse=True)
        tot, sup, fs, y = scored[0]
        second = scored[1][0] if len(scored) > 1 else 0
        if (sup >= 2 or (fs or 0) >= 0.5) and tot > second + 1.0:
            proposals.append((tot, x, y, sup, fs))
    proposals.sort(reverse=True)
    for tot, x, y, sup, fs in proposals:
        if x in pairs or y in taken: continue
        add(x, y, 'graph s%d f%.2f' % (sup, fs or 0)); added += 1
    if not added and not norder: break
print('after the call graph: %d pairs (%d rounds)' % (len(pairs), rnd + 1))

def consistency(sel):
    ok = tot = 0
    for pa, ga in pairs.items():
        if not sel(why[pa]): continue
        gcs = set(gcallees.get(ga, ()))
        for t in set(pcallees[pa]):
            if t in pairs: tot += 1; ok += pairs[t] in gcs
    return ok, tot
for label in ('anchor', 'accesses', 'data', 'args', 'graph', 'order', 'table', 'window'):
    ok, tot = consistency(lambda w: w.startswith(label))
    print('  %s pairs: callees agree %d/%d (%.0f%%)' % (label, ok, tot, 100.0 * ok / max(tot, 1)))

# ---- near copies: the same function compiled into several modules (stage modules each carry
# their own copy of common objects, not always byte-identical): a PC function is all of them ----
copy_index = collections.defaultdict(list)
for g in gt:
    if gf[g]['insns'] >= 12: copy_index[tuple(sorted(gt[g].items()))].append(g)
extra = collections.defaultdict(list)  # rep -> more copies
for pa, ga in pairs.items():
    key = tuple(sorted(gt[ga].items()))
    if not key: continue
    for y in copy_index.get(key, ()):
        if y == ga or y in taken or gf[y]['module'] == gf[ga]['module']: continue
        if abs(gf[y]['insns'] - gf[ga]['insns']) > 0.05 * gf[ga]['insns'] + 1: continue
        a, b = gacc[ga], gacc[y]
        if a or b:
            if len(a) * len(b) > 160000: continue
            n, m = len(a), len(b)
            prev = [0] * (m + 1)
            for i in range(n):
                cur = [0] * (m + 1)
                for j in range(m): cur[j + 1] = prev[j] + 1 if a[i] == b[j] else max(prev[j + 1], cur[j])
                prev = cur
            if 2.0 * prev[m] / max(1, n + m) < 0.85: continue
        extra[ga].append(y)
for ga, ys in extra.items():
    for y in ys:
        if y not in taken:
            taken[y] = taken[ga]
            members[ga] = sorted(set(members[ga]) | set(members[y]))
print('near copies added: %d' % sum(len(v) for v in extra.values()))

# Precision against ground truth: data blocks matched by content. A pair's PC function references
# such a block: its GameCube partner should reference the block's twin.
prec = collections.defaultdict(lambda: [0, 0])
for pa, ga in pairs.items():
    blocks = [v for v, sz, m, at in pf[pa]['globals'] if v in dpairs]
    if not blocks: continue
    gs = {v for mem in members[ga] for v, sz, m, at in gf[mem]['globals']}
    meth = why[pa].split()[0]
    prec[meth][1] += 1
    prec[meth][0] += any(dpairs[v] in gs for v in blocks)
print('precision on data ground truth: ' + ', '.join('%s %d/%d' % (m, a, b) for m, (a, b) in sorted(prec.items())))

# ---- 3. variables ----
votes = collections.defaultdict(collections.Counter)
sizes = collections.defaultdict(collections.Counter)
for pa, ga in pairs.items():
    pg = [(a, s) for a, s, m, at in pf[pa]['globals']]
    gg = [(a, s) for a, s, m, at in gf[ga]['globals']]
    if not pg or not gg or len(pg) * len(gg) > 40000: continue
    for i, j in lcs_align(pg, gg, lambda x, y: x[1] == y[1] or not x[1] or not y[1]):
        votes[pg[i][0]][gg[j][0]] += 1
        if pg[i][1] and gg[j][1]: sizes[pg[i][0]][gg[j][1]] += 1
data = {}
for a, v in votes.items():
    top = v.most_common(2)
    n0 = top[0][1]; n1 = top[1][1] if len(top) > 1 else 0
    if (n0 >= 2 and n0 >= 2 * n1) or (n0 == 1 and n1 == 0): data[a] = (top[0][0], n0, sum(v.values()))
# Data blocks matched by content are certain: they win, with their extent (up to the next block
# either side references) and element size (u16 tables matched with halves swapped).
psorted, gsorted = sorted(pref), sorted(gref)
def extent(sorted_list, a, lo, hi):
    import bisect as _b
    i = _b.bisect_right(sorted_list, a)
    nxt = sorted_list[i] if i < len(sorted_list) else a + 4096
    return max(4, min(nxt - a, hi))
block_size = {}
for v, g in dpairs.items():
    n = min(extent(psorted, v, 0, 4096), extent(gsorted, g, 0, 4096))
    data[v] = (g, 99, 99)
    block_size[v] = n & ~3
print('variables: %d' % len(data))

with open('build/pcmatch.tsv', 'w') as o:
    for pa in sorted(pairs):
        o.write('func\t%08X\t%s\t%s\n' % (pa, ','.join('%08X' % m for m in members[pairs[pa]]), why[pa]))
    for a in sorted(data):
        g0, n, tot = data[a]
        sz = block_size.get(a) or (sizes[a].most_common(1)[0][0] if sizes[a] else 0)
        o.write('data\t%08X\t%08X\t%d\t%d/%d\n' % (a, g0, sz, n, tot))

known = {0x4372E0: 0x8006B7EC, 0x7941C0: 0x80010B38}
print('check 00547440 (Chao tree walker) ->', ('%08X ' % pairs[0x547440] + why[0x547440]) if 0x547440 in pairs else 'unmatched')
for p, g in known.items():
    print('check %08X -> %s (want %08X) %s' % (p, ('%08X' % pairs[p]) if p in pairs else '-', g, why.get(p, '')))
seedf, seedd = P['seeds_f'], P['seeds_d']
print('header functions matched: %d of %d; header variables: %d of %d' % (
    sum(1 for a in pairs if a in seedf), len(seedf), sum(1 for a in data if a in seedd), len(seedd)))
