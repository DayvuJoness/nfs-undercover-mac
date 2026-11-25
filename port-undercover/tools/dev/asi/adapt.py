"""Write a copy of the Police Career plugin whose references to the retail exe point at our exe instead.
usage: adapt.py plugin.asi out.asi  (uses /tmp/pcport/{map,rev2,slot,refs}.json)"""
import os
import json, bisect, struct, sys, collections
asi = bytearray(open(sys.argv[1], 'rb').read())
m = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'map.json')))
F = sorted((r, a, s) for a, r, s in m['functions']); FR = [f[0] for f in F]
pairs = dict((int(k), v) for k, v in m['pairs'])
D = sorted((vr, vo) for vr, vo in pairs.items() if vr >= 0xbd7000); DR = [d[0] for d in D]
extra = {int(k): v for k, v in json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'rev2.json'))).items()}
extra.update({int(k): v for k, v in json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'slot.json'))).items()})
_vt_path = os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'vt.json')
VT = {int(k): v for k, v in json.load(open(_vt_path)).items()} if os.path.exists(_vt_path) else {}
_O = open(os.environ['OURS_EXE'], 'rb').read() if os.environ.get('OURS_EXE') else None
_R = open(os.environ['RETAIL_EXE'], 'rb').read() if os.environ.get('RETAIL_EXE') else None
def code_map(x):
    if x in VT: return VT[x]
    if x in extra: return extra[x]
    i = bisect.bisect_right(FR, x) - 1
    if i >= 0 and F[i][0] <= x < F[i][0] + F[i][2]: return F[i][1] + x - F[i][0]
    # between two matched functions shifted alike, in an unmatched one: the same shift, when 6 bytes agree
    if _O and _R and 0 <= i < len(F) - 1:
        d0, d1 = F[i][1] - F[i][0], F[i + 1][1] - F[i + 1][0]
        if d0 == d1:
            y = x + d0
            if _R[x - 0x400000:x - 0x400000 + 6] == _O[y - 0x400000:y - 0x400000 + 6]:
                return y
    return None
def data_map(x):
    if x in pairs: return pairs[x]
    i = bisect.bisect_right(DR, x) - 1
    if i < 0 or i + 1 >= len(D): return None
    (r0, o0), (r1, o1) = D[i], D[i + 1]
    if o0 - r0 == o1 - r1 or x - r0 < 0x40: return x + o0 - r0
    return None
_sec_path = os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'securom.json')
SECUROM = {int(k): v for k, v in json.load(open(_sec_path)).items()} if os.path.exists(_sec_path) else {}
_ovr_path = os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'overrides.json')
OVR = {int(k): v for k, v in json.load(open(_ovr_path)).items()} if os.path.exists(_ovr_path) else {}
def amap(x):
    if x in OVR: return OVR[x]
    if 0x1d22000 <= x < 0x3072000: return SECUROM.get(x)
    return code_map(x) if x < 0xbd7000 else data_map(x)
pe = struct.unpack_from('<I', asi, 0x3c)[0]; n = struct.unpack_from('<H', asi, pe + 6)[0]; optsz = struct.unpack_from('<H', asi, pe + 20)[0]
base = struct.unpack_from('<I', asi, pe + 24 + 28)[0]
secs = []
for i in range(n):
    o = pe + 24 + optsz + 40 * i
    vs, va, rs, ra = struct.unpack_from('<4I', asi, o + 8); secs.append((va, vs, ra, rs))
def rva2off(r):
    for va, vs, ra, rs in secs:
        if va <= r < va + rs: return ra + r - va
refs = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'refs.json')))
st = collections.Counter(); unres = collections.Counter()
def texty(v): return all(0x20 <= (v >> s) & 0xff < 0x7f for s in (0, 8, 16))
def printable(c): return 0x20 <= c < 0x7f or c in (9, 10, 13)
def in_text(off):
    # the field overlaps a real string by 2+ bytes: a printable run of 6+ bytes, or 3+ UTF-16 characters
    for k in range(off - 1, off + 4):
        if 0 <= k < len(asi) and printable(asi[k]):
            lo = k
            while lo > 0 and printable(asi[lo - 1]): lo -= 1
            hi = k
            while hi < len(asi) and printable(asi[hi]): hi += 1
            if hi - lo >= 6 and min(hi, off + 4) - max(lo, off) >= 2:
                return True
    # a short NUL-terminated string (".BIN", "auto", "long"): the field ends in its NUL after 3 printable bytes and
    # either a printable byte comes before it or the field is unaligned (pointers in data are 4-byte aligned); the
    # 2026-11-20 run had ".BIN" remapped to ".2FN" (0x004e4942 looks like a code address), so the Interceptor
    # gameplay file was not found and the game died allocating -76 bytes
    if 0 < off and off + 4 <= len(asi) and asi[off + 3] == 0 and all(printable(asi[off + j]) for j in range(3)) \
            and (printable(asi[off - 1]) or off % 4):
        return True
    def wch(p): return 0 <= p and p + 1 < len(asi) and printable(asi[p]) and asi[p + 1] == 0
    for k in range(off - 2, off + 4):
        if wch(k):
            a = k
            while wch(a - 2): a -= 2
            b = k
            while wch(b): b += 2
            if (b - a) // 2 >= 3 and min(b, off + 4) - max(a, off) >= 2:
                return True
    return False
for rva, v, kind, txt in refs:
    off = rva2off(rva)
    if kind in ('.rdata', '.data') and off is not None and in_text(off): st['in text, left'] += 1; continue
    if off is None: st['no file offset'] += 1; continue
    if struct.unpack_from('<I', asi, off)[0] != v and kind != 'rel': st['changed already'] += 1; continue
    t = amap(v)
    if t is None:
        st['text-like' if texty(v) else 'unresolved'] += 1
        if not texty(v): unres[v] += 1
        continue
    if kind == 'rel':
        struct.pack_into('<i', asi, off, t - (base + rva + 4))
    else:
        struct.pack_into('<I', asi, off, t)
    st['mapped'] += 1
print(st, 'distinct unresolved', len(unres))
open(sys.argv[2], 'wb').write(asi)
json.dump(sorted(unres), open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'unresolved.json'), 'w'))
