"""Map addresses of the retail nfs.exe 1.0.0.1 (46.6 MB) onto ours (10.5 MB) by matching every one of our
functions (masked: absolute addresses and rel32 fields) in the retail .text near where the previous one matched,
then pairing the absolute operands of matched instructions into a data map.
usage: exemap.py ours.exe retail.exe functions.tsv out.json"""
import os
import struct, sys, json, bisect, collections
from capstone import *
from capstone.x86 import *
ours_p, ret_p, ftsv, outp = sys.argv[1:5]
O = open(ours_p, 'rb').read(); R = open(ret_p, 'rb').read()
BASE = 0x400000
def sec_table(b):
    pe = struct.unpack_from('<I', b, 0x3c)[0]; n = struct.unpack_from('<H', b, pe + 6)[0]; optsz = struct.unpack_from('<H', b, pe + 20)[0]
    out = []
    for i in range(n):
        o = pe + 24 + optsz + 40 * i
        nm = b[o:o + 8].rstrip(b'\0').decode('latin1'); vs, va, rs, ra = struct.unpack_from('<4I', b, o + 8)
        out.append((nm, va, vs, ra, rs))
    return out
def rva2off(secs, rva):
    for nm, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs): return ra + rva - va if rva - va < rs else None
    return None
OS, RS = sec_table(O), sec_table(R)
# both .text at file offset == rva (raw 0x1000 at va 0x1000)
funcs = []
for line in open(ftsv):
    p = line.split('\t')
    if p[0] == 'address': continue
    funcs.append((int(p[0], 16), int(p[2])))
funcs.sort()
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
OLO, OHI = 0x400000, 0x1410000
def masked(addr, size):
    off = addr - BASE
    code = O[off:off + size]
    mask = bytearray(b'\xff' * len(code))
    fields = []  # (offset in function, kind)
    pos = 0
    for ins in md.disasm(code, addr):
        rel = ins.address - addr
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            if ins.size >= 5 and ins.operands and ins.operands[0].type == X86_OP_IMM:
                t = ins.operands[0].imm & 0xffffffff
                if not (addr <= t < addr + size):
                    k = ins.size - 4
                    mask[rel + k:rel + k + 4] = b'\0\0\0\0'; fields.append((rel + k, 'rel'))
        enc = ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size))
        for fo, fs in enc:
            if fo and fs == 4 and rel + fo + 4 <= len(code):
                v = struct.unpack_from('<I', code, rel + fo)[0]
                if OLO <= v < OHI and mask[rel + fo] == 0xff:
                    mask[rel + fo:rel + fo + 4] = b'\0\0\0\0'; fields.append((rel + fo, 'abs'))
        pos = rel + ins.size
    return code, bytes(mask), fields, pos
def longest_run(mask):
    best = (0, 0); s = None
    for i, m in enumerate(mask + b'\0'):
        if m == 0xff and s is None: s = i
        elif m != 0xff and s is not None:
            if i - s > best[1] - best[0]: best = (s, i)
            s = None
    return best
def matches_at(code, mask, roff):
    seg = R[roff:roff + len(code)]
    if len(seg) != len(code): return False
    for i in range(len(code)):
        if mask[i] == 0xff and seg[i] != code[i]: return False
    return True
fmap = []  # (ours, retail, size)
delta = 0; miss = 0; amb = 0
TEXT_END = 0x7d7000
for idx, (a, size) in enumerate(funcs):
    if size <= 0 or a - BASE + size > len(O): continue
    code, mask, fields, decoded = masked(a, size)
    s, e = longest_run(mask)
    if e - s < 6:
        # too little to search: accept only at the expected place
        roff = a - BASE + delta
        if matches_at(code, mask, roff): fmap.append((a, a + delta, size, fields))
        else: miss += 1
        continue
    key = code[s:e][:32]
    exp = a - BASE + delta
    found = []
    for win in (0x4000, 0x40000, 0x400000):
        lo = max(0x1000, exp - win); hi = min(TEXT_END, exp + win + len(code))
        p = R.find(key, lo, hi)
        while p >= 0 and len(found) < 4:
            roff = p - s
            if matches_at(code, mask, roff) and roff not in found: found.append(roff)
            p = R.find(key, p + 1, hi)
        if found: break
    if not found: miss += 1; continue
    if len(found) > 1:
        found.sort(key=lambda r: abs(r - exp))
        amb += 1
    roff = found[0]
    delta = roff - (a - BASE)
    fmap.append((a, roff + BASE, size, fields))
print('functions', len(funcs), 'matched', len(fmap), 'missed', miss, 'ambiguous (nearest taken)', amb)
# data pairs from matched abs fields
pairs = collections.defaultdict(collections.Counter)
for a, r, size, fields in fmap:
    for fo, kind in fields:
        vo = struct.unpack_from('<I', O, a - BASE + fo)[0]
        vr = struct.unpack_from('<I', R, r - BASE + fo)[0]
        if kind == 'rel':
            vo = (a + fo + 4 + vo) & 0xffffffff; vr = (r + fo + 4 + vr) & 0xffffffff
        pairs[vr][vo] += 1
conf = {}
bad = 0
for vr, c in pairs.items():
    (vo, n), = c.most_common(1)
    if len(c) > 1: bad += 1
    conf[vr] = vo
print('operand pairs', len(conf), 'conflicting', bad)
# a matched function whose own address operands disagree with what most functions say those addresses are is a
# wrong pick among look-alikes (they differ only in the addresses the mask hid): dropped, left to the later matchers
strength = {vr: c.most_common(1)[0][1] for vr, c in pairs.items()}
kept = []
dropped = 0
for a, r, size, fields in fmap:
    ok = True
    for fo, kind in fields:
        if kind != 'abs': continue
        vo = struct.unpack_from('<I', O, a - BASE + fo)[0]
        vr = struct.unpack_from('<I', R, r - BASE + fo)[0]
        if vr in conf and conf[vr] != vo and strength.get(vr, 0) >= 2:
            ok = False; break
    if ok: kept.append((a, r, size, fields))
    else: dropped += 1
fmap = kept
print('functions dropped for disagreeing operands', dropped)
json.dump({'functions': [(a, r, s) for a, r, s, _ in fmap], 'pairs': sorted(conf.items())}, open(outp, 'w'))
