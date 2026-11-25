"""List the Police Career plugin's hook patches whose expectation fails on our exe after the map.
Builders: 0x6c9c2550 pointer (kind 0), 0x6c9c25e0 pointer (kind 1), 0x6c9c2590 CALL rel32 (kind 2),
0x6c9c2620 JMP rel32 (kind 3); arguments (patch, site, expected original, replacement) on the stack.
usage: patchcheck.py plugin.asi ours.exe retail.exe   (WORK: map files)"""
import os, sys, json, struct, collections
from capstone import *
from capstone.x86 import *
asi_p, ours_p, ret_p = sys.argv[1:4]
A = open(asi_p, 'rb').read(); O = open(ours_p, 'rb').read(); R = open(ret_p, 'rb').read()
exec(open(os.path.join(os.path.dirname(__file__), 'adapt.py')).read().split("pe = struct.unpack_from")[0]
     .replace("asi = bytearray(open(sys.argv[1], 'rb').read())", ""))
if os.environ.get('ADAPTED'):
    # checking the adapted copy: its constants already name our addresses
    def amap(x): return x
BUILD = {0x6c9c2550: 'ptr', 0x6c9c25e0: 'ptr', 0x6c9c2590: 'call', 0x6c9c2620: 'jmp'}
pe = struct.unpack_from('<I', A, 0x3c)[0]; n = struct.unpack_from('<H', A, pe + 6)[0]; optsz = struct.unpack_from('<H', A, pe + 20)[0]
base = struct.unpack_from('<I', A, pe + 24 + 28)[0]
for i in range(n):
    o = pe + 24 + optsz + 40 * i
    if A[o:o + 8].rstrip(b'\0') == b'.text': vs, va, rs, ra = struct.unpack_from('<4I', A, o + 8)
def u32(b, a):
    o = a - 0x400000
    return struct.unpack_from('<I', b, o)[0] if 0 <= o < len(b) - 4 else None
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
code = A[ra:ra + vs]; off = 0; args = {}
st = collections.Counter(); bad = []
while off < len(code):
    got = False
    for ins in md.disasm(code[off:], base + va + off):
        got = True; off = ins.address - base - va + ins.size; ops = ins.operands
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_ESP \
                and ops[0].mem.index == 0 and ops[1].type == X86_OP_IMM:
            args[ops[0].mem.disp] = ops[1].imm & 0xffffffff
        elif ins.mnemonic == 'call':
            t = ops[0].imm & 0xffffffff if ops and ops[0].type == X86_OP_IMM else None
            if t in BUILD and 4 in args and 8 in args:
                site, orig = args[4], args[8]; kind = BUILD[t]
                y, e = amap(site), amap(orig)
                if y and not (0x401000 <= y < 0x1400000): y = None
                if not y: st[kind + ': site not mapped'] += 1; bad.append((kind, hex(ins.address), hex(site), 'site?', hex(orig), hex(e or 0)))
                elif kind == 'ptr':
                    v = u32(O, y)
                    if v == e: st[kind + ": ok"] += 1; (os.environ.get("PC_LIST") and print("ok", kind, hex(y)))
                    else: st[kind + ': differs'] += 1; bad.append((kind, hex(ins.address), hex(site), hex(y), hex(orig), hex(e or 0), 'ours has', hex(v or 0)))
                else:
                    ob = O[y - 0x400000:y - 0x400000 + 5]
                    real = (y + 5 + struct.unpack_from('<i', ob, 1)[0]) & 0xffffffff
                    if ob[0] in (0xe8, 0xe9) and real == e: st[kind + ': ok'] += 1
                    else: st[kind + ': differs'] += 1; bad.append((kind, hex(ins.address), hex(site), hex(y), hex(orig), hex(e or 0), 'ours', ob.hex(), hex(real)))
            args = {}
        elif ins.mnemonic in ('ret', 'jmp'):
            args = {}
    if not got: off += 1
print(dict(st))
for b in bad: print(' ', *b)
