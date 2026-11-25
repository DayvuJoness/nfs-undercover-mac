import os
import struct, sys, json, collections
from capstone import *
from capstone.x86 import *
asi = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', asi, 0x3c)[0]; n = struct.unpack_from('<H', asi, pe + 6)[0]; optsz = struct.unpack_from('<H', asi, pe + 20)[0]
base = struct.unpack_from('<I', asi, pe + 24 + 28)[0]
secs = []
for i in range(n):
    o = pe + 24 + optsz + 40 * i
    nm = asi[o:o + 8].rstrip(b'\0').decode(); vs, va, rs, ra = struct.unpack_from('<4I', asi, o + 8)
    secs.append((nm, va, vs, ra, rs))
# relocations (absolute refs inside the asi): their positions
relocs = set()
for nm, va, vs, ra, rs in secs:
    if nm == '.reloc':
        p = ra
        while p < ra + vs:
            pva, sz = struct.unpack_from('<II', asi, p)
            if sz == 0: break
            for k in range((sz - 8) // 2):
                e = struct.unpack_from('<H', asi, p + 8 + 2 * k)[0]
                if e >> 12 == 3: relocs.add(pva + (e & 0xfff))
            p += sz
LO, HI = 0x401000, 0x3080000
refs = []  # (asi rva of the 4 bytes, value, kind, insn text)
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
for nm, va, vs, ra, rs in secs:
    if nm != '.text': continue
    code = asi[ra:ra + vs]
    off = 0
    while off < len(code):
        got = False
        for ins in md.disasm(code[off:], base + va + off):
            got = True
            # find 4-byte fields holding values in range
            raw = bytes(ins.bytes)
            for fo, fs in ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size)):
                if fo and fs == 4 and fo + 4 <= len(raw):
                    v = struct.unpack_from('<I', raw, fo)[0]
                    rva = ins.address - base + fo
                    # a jump's or call's own displacement is relative (taken below); `jmp/call [address]` is not
                    branch = ins.mnemonic.startswith('j') or ins.mnemonic == 'call'
                    if LO <= v < HI and rva not in relocs and (not branch or fo == ins.disp_offset):
                        refs.append((rva, v, 'code', ins.mnemonic + ' ' + ins.op_str))
            # relative calls/jumps into the exe
            if ins.mnemonic in ('call', 'jmp') and ins.operands and ins.operands[0].type == X86_OP_IMM:
                t = ins.operands[0].imm & 0xffffffff
                if LO <= t < HI: refs.append((ins.address - base + 1, t, 'rel', ins.mnemonic))
            off = ins.address - base - va + ins.size
        if not got: off += 1
for nm, va, vs, ra, rs in secs:
    if nm in ('.rdata', '.data'):
        for k in range(0, vs - 3, 4):
            v = struct.unpack_from('<I', asi, ra + k)[0]
            if LO <= v < HI and (va + k) not in relocs:
                refs.append((va + k, v, nm, ''))
c = collections.Counter(k for _, _, k, _ in refs)
print(c, 'distinct values', len(set(v for _, v, _, _ in refs)))
json.dump(refs, open(sys.argv[2], 'w'))
