"""Reverse match retail code addresses in our exe, using known address pairs inside the pattern.
usage: revmatch2.py ours.exe retail.exe want.json out.json"""
import os
import json, bisect, struct, sys, collections
from capstone import *
from capstone.x86 import *
O = open(sys.argv[1], 'rb').read(); R = open(sys.argv[2], 'rb').read(); BASE = 0x400000
m = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'map.json')))
F = sorted((r, a, s) for a, r, s in m['functions']); FR = [f[0] for f in F]
pairs = dict((int(k), v) for k, v in m['pairs'])
D = sorted((vr, vo) for vr, vo in pairs.items() if vr >= 0xbd7000); DR = [d[0] for d in D]
extra = {}
_vt = os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'vt.json')
VT = {int(k): v for k, v in json.load(open(_vt)).items()} if os.path.exists(_vt) else {}
_ovr = os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'overrides.json')
OVR = {int(k): v for k, v in json.load(open(_ovr)).items()} if os.path.exists(_ovr) else {}
def code_map(x):
    if x in OVR: return OVR[x]
    if x in VT: return VT[x]
    if x in extra: return extra[x]
    i = bisect.bisect_right(FR, x) - 1
    if i >= 0 and F[i][0] <= x < F[i][0] + F[i][2]: return F[i][1] + x - F[i][0]
    return None
def data_map(x):
    if x in pairs: return pairs[x]
    i = bisect.bisect_right(DR, x) - 1
    if i < 0 or i + 1 >= len(D): return None
    (r0, o0), (r1, o1) = D[i], D[i + 1]
    if o0 - r0 == o1 - r1: return x + o0 - r0
    return None
def amap(x): return code_map(x) if x < 0xbd7000 else data_map(x)
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
def pattern(x, n=64):
    off = x - BASE; code = R[off:off + n + 16]
    want = []  # per byte: int or None
    fields = []  # (pos, kind, value)
    end = 0
    for ins in md.disasm(code, x):
        rel = ins.address - x
        if rel >= n: break
        bad = False
        flds = []
        if (ins.mnemonic == 'call' or ins.mnemonic.startswith('j')) and ins.size >= 5 and ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm & 0xffffffff
            if t >= 0x1389000: bad = True
            flds.append((rel + ins.size - 4, 'rel', t))
        for fo, fs in ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size)):
            if fo and fs == 4 and fo + 4 <= ins.size:
                v = struct.unpack_from('<I', code, rel + fo)[0]
                if 0x1389000 <= v < 0x3100000: bad = True
                if 0x400000 <= v < 0x1389000: flds.append((rel + fo, 'abs', v))
        if bad: break
        fields += flds
        end = rel + ins.size
        if ins.mnemonic in ('ret', 'jmp'): break
    return code[:end], fields
def try_at(st, code, fields):
    seg = O[st:st + len(code)]
    if len(seg) != len(code): return -1
    masked = set()
    for p, k, v in fields: masked.update(range(p, p + 4))
    for i in range(len(code)):
        if i not in masked and seg[i] != code[i]: return -1
    score = 0
    for p, k, v in fields:
        got = struct.unpack_from('<I', seg, p)[0]
        if k == 'rel':
            got = (st + BASE + p + 4 + got) & 0xffffffff
        e = amap(v)
        if e is not None:
            if got != e: return -1
            score += 1
        elif k == 'rel' and 0x401000 <= v < 0xbd7000 and 0x401000 <= got < 0xbd6000:
            # a target the map lacks (a tiny getter with many twins): its first bytes must agree
            rb = R[v - BASE:v - BASE + 6]; ob = O[got - BASE:got - BASE + 6]
            if rb == ob: score += 1
    return score
def push_jmp_call(x):
    # SecuROM's `push <return>; jmp <target>` in place of `call <target>`: ours has the CALL itself, after the
    # same instructions as in the retail exe. Searched in all of our code (local order differs between builds).
    off = x - BASE
    if R[off] == 0xe8:
        # a plain CALL in a function the matchers missed: found the same way, by what precedes it (its target
        # may be unknown too: then any CALL after a unique run of the same instructions)
        t = (x + 5 + struct.unpack_from('<i', R, off + 1)[0]) & 0xffffffff
        tt = amap(t)
        start = off
        # the target's own mapping may be the wrong look-alike: then any CALL after the same instructions, and
        # what that CALL reaches in our build is the target (kept for the map: callfix.json)
        r_ = prefix_call(x, start, tt)
        if r_ or not tt: return r_
        r_ = prefix_call(x, start, None)
        if r_ and O[r_ - BASE] == 0xe8:
            CALLFIX[t] = (r_ + 5 + struct.unpack_from('<i', O, r_ - BASE + 1)[0]) & 0xffffffff
        return r_
    if R[off] != 0xe9 or R[off - 5] != 0x68: return None
    ret = struct.unpack_from('<I', R, off - 4)[0]
    if not (x < ret < x + 0x40): return None
    t = (x + 5 + struct.unpack_from('<i', R, off + 1)[0]) & 0xffffffff
    tt = amap(t)
    if not tt: return None
    return prefix_call(x, off - 5, tt)
def _nearest(x, hits):
    # among several equal candidates, the one exactly where the neighbours put it
    i = bisect.bisect_right(FR, x) - 1
    if i < 0 or len(hits) < 2: return None
    exp = x + F[i][1] - F[i][0]
    hs = sorted(hits, key=lambda h: abs(h - exp))
    if abs(hs[0] - exp) <= 0x40 and abs(hs[1] - exp) >= 0x100: return hs[0]
    return None
def prefix_call(x, start, tt):
    # the instructions before: the shortest run of whole instructions (8+ bytes) that is unique in ours
    for back in range(8, 33):
        cand = start - back
        insns = list(md.disasm(R[cand:start], cand + BASE))
        if not insns or insns[-1].address + insns[-1].size != start + BASE or sum(i.size for i in insns) != back:
            continue
        prefix = R[cand:start]
        hits = []
        p = O.find(prefix, 0x1000, 0x7d6000)
        while p >= 0 and len(hits) < 8:
            q = p + len(prefix)
            if O[q] == 0xe8 and (tt is None or (q + BASE + 5 + struct.unpack_from('<i', O, q + 1)[0]) & 0xffffffff == tt):
                hits.append(q + BASE)
            p = O.find(prefix, p + 1, 0x7d6000)
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1 and _nearest(x, hits):
            return _nearest(x, hits)
        if not hits:
            continue  # a run decoded from the middle of an instruction: try a longer one
    # the same with short jumps' displacements left open (a jump across the moved call is shorter in ours)
    import re as _re
    for back in range(8, 33):
        cand = start - back
        insns = list(md.disasm(R[cand:start], cand + BASE))
        if not insns or sum(i_.size for i_ in insns) != back: continue
        pat = b''
        for i_ in insns:
            raw = bytearray(i_.bytes)
            if i_.size == 2 and (raw[0] & 0xf0) == 0x70 or raw[0] == 0xeb and i_.size == 2:
                pat += _re.escape(bytes(raw[:1])) + b'.'
                continue
            # absolute operands: through the map, or left open
            holes = []
            for fo, fs in ((i_.imm_offset, i_.imm_size), (i_.disp_offset, i_.disp_size)):
                if fo and fs == 4:
                    v_ = struct.unpack_from('<I', bytes(raw), fo)[0]
                    if 0x400000 <= v_ < 0x3100000:
                        mv = amap(v_) if v_ < 0x1389000 else None
                        if mv: struct.pack_into('<I', raw, fo, mv)
                        else: holes.append(fo)
            k_ = 0; part = b''
            while k_ < len(raw):
                if k_ in holes: part += b'....'; k_ += 4
                else: part += _re.escape(bytes(raw[k_:k_ + 1])); k_ += 1
            pat += part
        rx = _re.compile(pat + b'\xe8', _re.S)
        hits = []
        for m_ in rx.finditer(O, 0x1000, 0x7d6000):
            q = m_.end() - 1
            if tt is None or (q + BASE + 5 + struct.unpack_from('<i', O, q + 1)[0]) & 0xffffffff == tt:
                hits.append(q + BASE)
            if len(hits) > 4: break
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1 and _nearest(x, hits):
            return _nearest(x, hits)
        if not hits:
            continue  # a run decoded from the middle of an instruction: try a longer one
    # a short prefix (the instructions right before, when a jump across the moved call sits further back),
    # taken only near where the neighbours put it
    i = bisect.bisect_right(FR, x) - 1
    exp = x + ((F[i][1] - F[i][0]) if i >= 0 else 0)
    for back in range(2, 8):
        cand = start - back
        insns = list(md.disasm(R[cand:start], cand + BASE))
        if not insns or sum(i_.size for i_ in insns) != back: continue
        prefix = R[cand:start]
        lo_, hi_ = max(0x1000, exp - BASE - 0x4000), min(0x7d6000, exp - BASE + 0x4000)
        hits = []
        p = O.find(prefix, lo_, hi_)
        while p >= 0:
            q = p + len(prefix)
            if O[q] == 0xe8 and (tt is None or (q + BASE + 5 + struct.unpack_from('<i', O, q + 1)[0]) & 0xffffffff == tt):
                hits.append(q + BASE)
            p = O.find(prefix, p + 1, hi_)
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1 and _nearest(x, hits):
            return _nearest(x, hits)
    return None
def twin(x, code, fields):
    # a tiny function with no address in it (a getter): the linker of our build may have folded it into one
    # identical copy elsewhere, which does the same thing; take the nearest identical copy after padding
    if fields or not code or code[-1] not in (0xc3, 0xc2) or len(code) > 16: return None
    i = bisect.bisect_right(FR, x) - 1
    exp = x + ((F[i][1] - F[i][0]) if i >= 0 else 0)
    best = None
    p = O.find(code, 0x1000, 0x7d6000)
    while p >= 0:
        a = p + BASE
        if O[p - 1] in (0xcc, 0x90, 0xc3) and (a & 0xf) == 0 and (best is None or abs(a - exp) < abs(best - exp)):
            best = a
        p = O.find(code, p + 1, 0x7d6000)
    return best
def imm_operand(x):
    # x is the imm32 of `mov r32, imm32` / `push imm32` naming a function: ours has the same instruction with the
    # mapped address (unique)
    off = x - BASE
    op = R[off - 1]
    if not (0xb8 <= op <= 0xbf or op == 0x68): return None
    v = struct.unpack_from('<I', R, off)[0]
    if not (0x401000 <= v < 0xbd7000): return None
    mv = amap(v)
    if not mv: return None
    pat = bytes([op]) + struct.pack('<I', mv)
    hits = []
    p = O.find(pat, 0x1000, 0x7d6000)
    while p >= 0 and len(hits) < 3:
        hits.append(p + 1 + BASE); p = O.find(pat, p + 1, 0x7d6000)
    return hits[0] if len(hits) == 1 else None
def search(x):
    im = imm_operand(x)
    if im: return im, 'ok'
    pj = push_jmp_call(x)
    if pj: return pj, 'ok'
    th = via_thunk(x)
    if th: return th, 'thunk'
    cl = callers(x)
    if cl and O[cl - BASE - 1] in (0xcc, 0x90, 0xc3, 0xc2, 0x00) and (cl & 0x3) == 0:
        return cl, 'callers'
    if R[x - BASE - 1] in (0xcc, 0x90, 0xc3) and (x & 0xf) == 0:
        fc = via_found_caller(x)
        if fc and O[fc - BASE - 1] in (0xcc, 0x90, 0xc3, 0xc2, 0x00):
            return fc, 'found caller'
    code, fields = pattern(x)
    if R[x - BASE - 1] in (0xcc, 0x90, 0xc3) and (x & 0xf) == 0:
        tw = twin(x, code, fields)
        if tw: return tw, 'twin'
    # a short pattern (a common prologue alone, SecuROM moved the rest away) is trusted only right where the
    # neighbours say it should be
    if len(code) < 3: return None, 'short'
    near_only = len(code) < 12
    i = bisect.bisect_right(FR, x) - 1
    d = (F[i][1] - F[i][0]) if i >= 0 else 0
    exp = x + d - BASE
    masked = set()
    for p, k, v in fields: masked.update(range(p, p + 4))
    runs = []; s = None
    for kk in range(len(code) + 1):
        um = kk < len(code) and kk not in masked
        if um and s is None: s = kk
        elif not um and s is not None: runs.append((kk - s, s)); s = None
    if not runs: return None, 'masked'
    ln, s = max(runs); key = code[s:s + ln]
    # absolute operands the map knows make a far more specific key (an SEH prologue names its own scope table)
    concrete = bytearray(code); known = set(range(len(code))) - masked
    for p_, k_, v_ in fields:
        if k_ == 'abs':
            e_ = amap(v_)
            if e_ is not None:
                struct.pack_into('<I', concrete, p_, e_); known.update(range(p_, p_ + 4))
    runs2 = []; s2 = None
    for kk in range(len(code) + 1):
        if kk < len(code) and kk in known and s2 is None: s2 = kk
        elif not (kk < len(code) and kk in known) and s2 is not None: runs2.append((kk - s2, s2)); s2 = None
    if runs2 and max(runs2)[0] > ln:
        ln, s = max(runs2); key = bytes(concrete[s:s + ln])
    for win in ((0x200,) if near_only else (0x2000, 0x20000, 0x200000)):
        found = []
        lo = max(0x1000, exp - win); hi = min(0x7d6000, exp + win)
        p = O.find(key, lo, hi)
        while p >= 0 and len(found) < 50:
            st = p - s
            sc = try_at(st, code, fields)
            unmasked = len(code) - len(masked)
            if sc >= 0 and (unmasked >= 4 or sc >= 1): found.append((sc, -abs(st - exp), st))
            p = O.find(key, p + 1, hi)
        if found:
            found.sort(reverse=True)
            # unique, or better scored, or exactly where the neighbours put it (code repeated inside one function)
            if len(found) == 1 or found[0][0] > found[1][0] or (found[0][0] == found[1][0] and found[0][1] == 0) or (found[0][0] == found[1][0] and found[0][0] >= 1 and abs(found[0][1]) < 0x100 and abs(found[1][1]) > 0x1000):
                return found[0][2] + BASE, 'ok'
            return None, 'ambiguous'
    return None, 'notfound'
# retail CALL sites by target, inside functions the forward match paired (for callers())
CALLS = collections.defaultdict(list)
for r0, a0, sz in F:
    o = r0 - BASE
    seg = R[o:o + sz]
    for k in range(len(seg) - 4):
        if seg[k] == 0xe8:
            t = (r0 + k + 5 + struct.unpack_from('<i', seg, k + 1)[0]) & 0xffffffff
            if 0x401000 <= t < 0xbd7000:
                CALLS[t].append((r0 + k, a0 + k))
# retail JMP thunks by target (all of .text: a thunk is often its own tiny function)
JMPS = collections.defaultdict(list)
for q in range(0x1000, 0x7d7000 - 5):
    if R[q] == 0xe9:
        t = (q + BASE + 5 + struct.unpack_from('<i', R, q + 1)[0]) & 0xffffffff
        if 0x401000 <= t < 0xbd7000 and R[q - 1] in (0xcc, 0x90, 0xc3):
            JMPS[t].append(q + BASE)
ALLCALLS = collections.defaultdict(list)
for q in range(0x1000, 0x7d7000 - 5):
    if R[q] == 0xe8:
        t = (q + BASE + 5 + struct.unpack_from('<i', R, q + 1)[0]) & 0xffffffff
        if 0x401000 <= t < 0xbd7000:
            ALLCALLS[t].append(q + BASE)
def via_found_caller(x):
    # a call to x from a place the matchers missed, found by what precedes it: what our CALL there reaches
    for site in ALLCALLS.get(x, ())[:6]:
        y = prefix_call(site, site - BASE, None)
        if y and O[y - BASE] == 0xe8:
            return (y + 5 + struct.unpack_from('<i', O, y - BASE + 1)[0]) & 0xffffffff
    return None
def via_thunk(x):
    # a retail `jmp x` thunk our build has too (mapped): where our thunk jumps is ours x
    for j in JMPS.get(x, ()):
        y = amap(j)
        if y and O[y - BASE] == 0xe9:
            return (y + 5 + struct.unpack_from('<i', O, y - BASE + 1)[0]) & 0xffffffff
    return None
def callers(x):
    # what our build calls from the places the retail exe calls x from (functions SecuROM hid, folded copies)
    votes = collections.Counter()
    for rs, os_ in CALLS.get(x, ()):
        if O[os_ - BASE] != 0xe8: continue
        votes[(os_ + 5 + struct.unpack_from('<i', O, os_ - BASE + 1)[0]) & 0xffffffff] += 1
    if not votes: return None
    (t, n), = votes.most_common(1)
    return t if n >= 2 or len(votes) == 1 else None
CALLFIX = {}
want = json.load(open(sys.argv[3]))
st = collections.Counter(); out = {}
for rounds in range(3):
    for x in want:
        if x in out: continue
        r, how = search(x)
        if rounds == 2: st[how] += 1
        if r: out[x] = r; extra[x] = r
print(st, 'resolved', len(out), 'of', len(want))
json.dump(out, open(sys.argv[4], 'w'))
json.dump({str(k): v for k, v in CALLFIX.items()}, open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'callfix.json'), 'w'))
