import os
import json, bisect, collections, struct
m = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'map.json')))
F = sorted((r, a, s) for a, r, s in m['functions'])
FR = [f[0] for f in F]
pairs = dict((int(k), v) for k, v in m['pairs'])
def code_map(x):
    i = bisect.bisect_right(FR, x) - 1
    if i >= 0 and F[i][0] <= x < F[i][0] + F[i][2]: return F[i][1] + x - F[i][0]
    return None
D = sorted((vr, vo) for vr, vo in pairs.items() if vr >= 0xbd7000)
deltas = collections.Counter()
for vr, vo in D: deltas[(vo - vr)] += 1
print('data deltas top', [(hex(d & 0xffffffff), n) for d, n in deltas.most_common(12)])
DR = [d[0] for d in D]
def data_map(x):
    if x in pairs: return pairs[x], 'exact'
    i = bisect.bisect_right(DR, x) - 1
    if i < 0 or i + 1 >= len(D): return None, 'none'
    (r0, o0), (r1, o1) = D[i], D[i + 1]
    if o0 - r0 == o1 - r1: return x + o0 - r0, 'between'
    if x - r0 < 0x40: return x + o0 - r0, 'near'
    return None, 'gap'
refs = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'refs.json')))
vals = collections.Counter()
res = collections.Counter(); unres = collections.Counter()
for rva, v, kind, txt in refs:
    if v < 0xbd7000:
        r = code_map(v); res['code ' + ('ok' if r else 'miss')] += 1
        if not r: unres[('code', v)] += 1
    elif v < 0x1386000:
        r, how = data_map(v); res['data ' + how] += 1
        if r is None: unres[('data', v)] += 1
    else:
        res['beyond'] += 1
print(res)
print('distinct unresolved', len(unres)); print([ (k, hex(v)) for (k, v), n in unres.most_common(30)])
