"""Pair functions through vtables (2026-11-19): a retail .rdata dword that is a function start, at an address the
data map pairs with ours, names the function our dword there names - when both functions have the same first
bytes. Tiny functions (getters, setters) are often folded or duplicated, where byte matching guessed; a vtable
slot is the reliable pairing. usage: vtmatch.py ours.exe retail.exe functions.tsv out.json  (WORK: map.json)"""
import os, sys, json, struct, bisect, collections
O = open(sys.argv[1], 'rb').read(); R = open(sys.argv[2], 'rb').read(); BASE = 0x400000
m = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'map.json')))
F = sorted((r, a, s) for a, r, s in m['functions']); FR = [f[0] for f in F]
ours_starts = set()
sizes = {}
for line in open(sys.argv[3]):
    p = line.split('\t')
    if p[0] != 'address':
        ours_starts.add(int(p[0], 16)); sizes[int(p[0], 16)] = int(p[2])
pairs = dict((int(k), v) for k, v in m['pairs'])
D = sorted((vr, vo) for vr, vo in pairs.items() if vr >= 0xbd7000); DR = [d[0] for d in D]
def data_map(x):
    if x in pairs: return pairs[x]
    i = bisect.bisect_right(DR, x) - 1
    if i < 0 or i + 1 >= len(D): return None
    (r0, o0), (r1, o1) = D[i], D[i + 1]
    if o0 - r0 == o1 - r1: return x + o0 - r0
    return None
retail_starts = set(r for r, a, s in F)
votes = collections.defaultdict(collections.Counter)
for a in range(0xbd7000, 0xd20000, 4):
    v = struct.unpack_from('<I', R, a - BASE)[0]
    if not (0x401000 <= v < 0xbd7000): continue
    y = data_map(a)
    if not y or not (0xbd6000 <= y < 0xd1f000): continue
    w = struct.unpack_from('<I', O, y - BASE)[0]
    if w not in ours_starts and not (0x401000 <= w < 0xbd6000 and O[w - BASE - 1] in (0xcc, 0x90, 0xc3)): continue
    n = min(12, sizes.get(w, 12))
    rb = bytearray(R[v - BASE:v - BASE + n]); ob = bytearray(O[w - BASE:w - BASE + n])
    # addresses inside the first bytes (an SEH prologue's handler, a global) differ between the builds
    for k in range(0, n - 3):
        if 0x401000 <= struct.unpack_from('<I', rb, k)[0] < 0xd30000:
            rb[k:k + 4] = ob[k:k + 4] = b'\0\0\0\0'
    if rb == ob or (R[v - BASE] == 0xe9 and O[w - BASE] == 0xe9):
        # same bytes, or both a JMP thunk (its displacement differs with the layout)
        votes[v][w] += 1
out = {}
for v, c in votes.items():
    (w, k), = c.most_common(1)
    if k >= 1 and (len(c) == 1 or k > c.most_common(2)[1][1]):
        out[v] = w
print('vtable pairs', len(out))
json.dump(out, open(sys.argv[4], 'w'))
