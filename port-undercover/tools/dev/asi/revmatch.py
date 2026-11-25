"""For retail code addresses the forward match missed: search our exe for the retail bytes there (masked)."""
import os
import json, bisect, struct, sys, collections
from capstone import *
from capstone.x86 import *
O = open(sys.argv[1], 'rb').read(); R = open(sys.argv[2], 'rb').read(); BASE = 0x400000
m = json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'map.json')))
F = sorted((r, a, s) for a, r, s in m['functions']); FR = [f[0] for f in F]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
def masked_retail(x, n=48):
    off = x - BASE; code = R[off:off + n + 16]; mask = bytearray(b'\xff' * len(code)); end = 0
    for ins in md.disasm(code, x):
        rel = ins.address - x
        if rel >= n: break
        if (ins.mnemonic == 'call' or ins.mnemonic.startswith('j')) and ins.size >= 5 and ins.operands and ins.operands[0].type == X86_OP_IMM:
            mask[rel + ins.size - 4:rel + ins.size] = b'\0' * 4
        for fo in set((ins.imm_offset, ins.disp_offset)):
            if fo and rel + fo + 4 <= len(code):
                v = struct.unpack_from('<I', code, rel + fo)[0]
                if 0x400000 <= v < 0x3100000: mask[rel + fo:rel + fo + 4] = b'\0' * 4
        if ins.mnemonic in ('ret', 'jmp') : end = rel + ins.size; break
        end = rel + ins.size
    return code[:end], bytes(mask[:end])
def search(x):
    code, mask = masked_retail(x)
    if len(code) < 3: return None, 'short'
    i = bisect.bisect_right(FR, x) - 1
    d = (F[i][1] - F[i][0]) if i >= 0 else 0
    exp = x + d - BASE
    runs = []; s = None
    for k, mm in enumerate(mask + b'\0'):
        if mm == 0xff and s is None: s = k
        elif mm != 0xff and s is not None: runs.append((k - s, s)); s = None
    if not runs: return None, 'masked'
    ln, s = max(runs); key = code[s:s + ln]
    for win in (0x2000, 0x20000, 0x200000):
        found = []
        lo = max(0x1000, exp - win); hi = min(0x7d6000, exp + win)
        p = O.find(key, lo, hi)
        while p >= 0 and len(found) < 3:
            st = p - s; seg = O[st:st + len(code)]
            if all(mask[k] != 0xff or seg[k] == code[k] for k in range(len(code))): found.append(st)
            p = O.find(key, p + 1, hi)
        if len(found) == 1: return found[0] + BASE, 'win%x' % win
        if len(found) > 1:
            found.sort(key=lambda q: abs(q - exp))
            if abs(found[0] - exp) < 0x40 and abs(found[1] - exp) > 0x400: return found[0] + BASE, 'nearest'
            return None, 'ambiguous'
    return None, 'notfound'
want = [int(v, 16) for v in sys.argv[3:]] if len(sys.argv) > 3 else json.load(open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'want.json')))
out = {}; st = collections.Counter()
for x in want:
    r, how = search(x); st[how] += 1
    if r: out[x] = r
print(st)
json.dump(out, open(os.path.join(os.environ.get('WORK', '/tmp/pcport'), 'rev.json'), 'w'))
