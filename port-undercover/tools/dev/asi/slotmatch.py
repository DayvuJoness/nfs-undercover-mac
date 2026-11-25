"""Map retail function starts the byte matchers could not (SecuROM hid them) by their slot between matched
neighbours: functions keep their order in both builds. usage: slotmatch.py ours retail functions.tsv want.json rev2.json out.json"""
import os
import json, bisect, sys, struct
O = open(sys.argv[1], 'rb').read(); R = open(sys.argv[2], 'rb').read(); BASE = 0x400000
ours = []
for line in open(sys.argv[3]):
    p = line.split('\t')
    if p[0] != 'address': ours.append((int(p[0], 16), int(p[2])))
ours.sort(); OA = [a for a, _ in ours]
m = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'map.json')))
F = sorted((r, a, s) for a, r, s in m['functions']); FR = [f[0] for f in F]
rev = {int(k): v for k, v in json.load(open(sys.argv[5])).items()}
want = [x for x in json.load(open(sys.argv[4])) if x not in rev]
def retail_starts(lo, hi):
    # aligned addresses after int3/nop padding or right after a ret/jmp, within [lo, hi)
    out = []
    for a in range(lo, hi):
        if a & 0xf: continue
        prev = R[a - BASE - 1]
        if prev in (0xcc, 0x90, 0xc3) or R[a - BASE - 3] == 0xc2:
            if R[a - BASE] not in (0xcc,): out.append(a)
    return out
res = {}; why = {}
for x in want:
    i = bisect.bisect_right(FR, x) - 1
    if i < 0 or i + 1 >= len(F): continue
    P, N = F[i], F[i + 1]
    if x < P[0] + P[2]: why[x] = 'inside matched'; continue
    rs = retail_starts(P[0] + P[2], N[0])
    oi, oj = bisect.bisect_left(OA, P[1] + 1), bisect.bisect_left(OA, N[1])
    os_ = [a for a, _ in ours[oi:oj] if a >= P[1] + P[2]]
    if x in rs and len(rs) == len(os_):
        res[x] = os_[rs.index(x)]
    elif len(os_) == 1 and rs and x == rs[0]:
        # one function of ours in the gap and x is the first start in the retail gap (SecuROM's bytes after a
        # moved prologue can look like more starts)
        res[x] = os_[0]
    else:
        why[x] = 'slots %d vs %d%s' % (len(rs), len(os_), '' if x in rs else ' (x not a start)')
print('slot-mapped', len(res), 'of', len(want))
import collections; print(collections.Counter(v.split(' (')[0] if 'slots' not in v else ('slots-equal' if v.split()[1]==v.split()[3] else 'slots-differ') for v in why.values()))
json.dump(res, open(sys.argv[6], 'w'))
