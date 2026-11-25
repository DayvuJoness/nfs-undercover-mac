"""Rewrite the Police Career plugin's byte signatures for our exe (2026-11-19).
The plugin checks game code before hooking it: `mov [esp], X; call addr_of (0x6c9bf650); ... mov esi, eax;
cmp dword ptr [esi + d], imm`. This follows that chain in each instruction run, keeps only the checks that hold
on the retail exe (its bytes at X + d equal imm), and writes our exe's bytes at map(X) + d into the adapted copy.
usage: sigfix.py original.asi adapted.asi retail.exe ours.exe   (WORK: the map files; rewrites adapted.asi)"""
import os, sys, json, struct, bisect, collections
from capstone import *
from capstone.x86 import *
orig_p, adapted_p, ret_p, ours_p = sys.argv[1:5]
A = open(orig_p, 'rb').read(); B = bytearray(open(adapted_p, 'rb').read())
R = open(ret_p, 'rb').read(); O = open(ours_p, 'rb').read()
WORK = os.environ.get('WORK', '/tmp/pcport')
exec(open(os.path.join(os.path.dirname(__file__), 'adapt.py')).read().split("pe = struct.unpack_from")[0]
     .replace("asi = bytearray(open(sys.argv[1], 'rb').read())", ""))
pe = struct.unpack_from('<I', A, 0x3c)[0]; n = struct.unpack_from('<H', A, pe + 6)[0]; optsz = struct.unpack_from('<H', A, pe + 20)[0]
base = struct.unpack_from('<I', A, pe + 24 + 28)[0]
text = None
for i in range(n):
    o = pe + 24 + optsz + 40 * i
    if A[o:o + 8].rstrip(b'\0') == b'.text':
        vs, va, rs, ra = struct.unpack_from('<4I', A, o + 8); text = (va, vs, ra)
ADDR_OF = 0x6c9bf650
def exe_bytes(b, a, size):
    off = a - 0x400000
    return b[off:off + size] if 0 <= off < len(b) - size else None
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
va, vs, ra = text
code = A[ra:ra + vs]
regs = {}       # register id -> retail address it holds
branch_regs = {}  # branch target -> registers at the branch
last_arg = None  # constant last stored to [esp]
st = collections.Counter(); fixes = []
off = 0
while off < len(code):
    got = False
    for ins in md.disasm(code[off:], base + va + off):
        got = True
        off = ins.address - base - va + ins.size
        mn = ins.mnemonic; ops = ins.operands
        # a branch target reached after a RET/JMP in the linear sweep gets the registers the branch saw
        if not regs and ins.address in branch_regs:
            regs = dict(branch_regs[ins.address])
        if mn.startswith('j') and mn != 'jmp' and ops and ops[0].type == X86_OP_IMM and regs:
            branch_regs.setdefault(ops[0].imm & 0xffffffff, dict(regs))
        if mn == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_ESP \
                and ops[0].mem.disp == 0 and ops[0].mem.index == 0 and ops[1].type == X86_OP_IMM:
            last_arg = ops[1].imm & 0xffffffff
            continue
        if mn == 'call':
            # a call keeps the callee-saved registers (cdecl/stdcall/thiscall all do)
            regs = {r: v for r, v in regs.items() if r in (X86_REG_EBX, X86_REG_ESI, X86_REG_EDI, X86_REG_EBP)}
            if ops and ops[0].type == X86_OP_IMM and (ops[0].imm & 0xffffffff) == ADDR_OF and last_arg:
                regs[X86_REG_EAX] = last_arg
            continue
        if mn in ('ret', 'jmp') or mn.startswith('j'):
            if mn in ('ret', 'jmp'): regs = {}; last_arg = None
            continue
        if mn == 'cmp' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_IMM \
                and ops[0].mem.base in regs and ops[0].mem.index == 0:
            size = ops[0].size
            x = (regs[ops[0].mem.base] + ops[0].mem.disp) & 0xffffffff
            imm = ops[1].imm & ((1 << (8 * size)) - 1)
            rb = exe_bytes(R, x, size)
            if rb is None or int.from_bytes(rb, 'little') != imm:
                st['does not hold on retail'] += 1
            else:
                y = amap(regs[ops[0].mem.base])
                ob = exe_bytes(O, y + ops[0].mem.disp, size) if y else None
                if ob is None:
                    st['address not mapped'] += 1
                    if os.environ.get('SIGFIX_LOG'): print('  not mapped: check at %08x of %08x' % (ins.address, x))
                else:
                    # the immediate is the last `size` bytes of the instruction
                    fo = ra + ins.address - base - va + ins.size - size
                    if A[fo:fo + size] != rb:
                        st['immediate not at the end'] += 1
                    else:
                        st['same in ours' if ob == rb else 'rewritten'] += 1
                        B[fo:fo + size] = ob
                        if ob != rb:
                            fixes.append((hex(ins.address), hex(x), rb.hex(), ob.hex()))
            continue
        # register bookkeeping: copies, lea/add with constants; any other write kills the register
        if mn == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG:
            if ops[1].reg in regs: regs[ops[0].reg] = regs[ops[1].reg]
            else: regs.pop(ops[0].reg, None)
            continue
        if mn == 'lea' and ops[1].mem.base in regs and ops[1].mem.index == 0:
            regs[ops[0].reg] = (regs[ops[1].mem.base] + ops[1].mem.disp) & 0xffffffff
            continue
        if mn == 'add' and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM and ops[0].reg in regs:
            regs[ops[0].reg] = (regs[ops[0].reg] + ops[1].imm) & 0xffffffff
            continue
        try:
            _, written = ins.regs_access()
            for r in written: regs.pop(r, None)
        except CsError:
            regs = {}
    if not got:
        off += 1
# memcmp signatures: `mov [esp], X; call addr_of; ... mov [esp+8], N; mov [esp+4], BUF; mov [esp], <reg>; call memcmp`
MEMCMP = 0x6c9d91b0
# the plugin's small checkN(X, BUF) helpers: `push ebx; sub esp,0x18; mov eax,[esp+0x20]; mov [esp],eax;
# call addr_of; mov dword ptr [esp+4], N; ...` - found by that prologue, N read from it
CHECKN_FUNCS = {}
_pro = bytes.fromhex('5383ec188b442420890424e8')
_p = code.find(_pro)
while _p >= 0:
    if code[_p + 16:_p + 20] == bytes.fromhex('c7442404'):
        CHECKN_FUNCS[base + va + _p] = struct.unpack_from('<I', code, _p + 20)[0]
    _p = code.find(_pro, _p + 1)
mc = collections.Counter()
off = 0
pending_x = None; args = {}
while off < len(code):
    got = False
    for ins in md.disasm(code[off:], base + va + off):
        got = True
        off = ins.address - base - va + ins.size
        ops = ins.operands
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_ESP \
                and ops[0].mem.index == 0 and ops[1].type == X86_OP_IMM:
            args[ops[0].mem.disp] = ops[1].imm & 0xffffffff
            continue
        if ins.mnemonic == 'call' and ops and ops[0].type == X86_OP_IMM:
            t = ops[0].imm & 0xffffffff
            CHECKN = CHECKN_FUNCS
            if t in CHECKN and 0 in args and 4 in args and 0x6c980000 <= args[4] < 0x6ca30000:
                # checkN(X, BUF): the N bytes at X equal BUF's
                x, buf, size = args[0], args[4], CHECKN[t]
                fo_b = None
                for i_ in range(n):
                    o_ = pe + 24 + optsz + 40 * i_
                    vs2, va2, rs2, ra2 = struct.unpack_from('<4I', A, o_ + 8)
                    if va2 <= buf - base < va2 + rs2: fo_b = ra2 + buf - base - va2
                rb = exe_bytes(R, x, size)
                if fo_b is not None and rb == A[fo_b:fo_b + size]:
                    y = amap(x)
                    ob = exe_bytes(O, y, size) if y else None
                    if ob:
                        B[fo_b:fo_b + size] = ob
                        mc['checkN: rewritten' if ob != rb else 'checkN: same in ours'] += 1
                    else:
                        mc['checkN: address not mapped'] += 1
            if t == ADDR_OF and 0 in args:
                pending_x = args[0]
            elif t == MEMCMP and pending_x and 4 in args and 8 in args and 0x6c980000 <= args[4] < 0x6ca30000:
                x, buf, size = pending_x, args[4], args[8]
                if 1 <= size <= 64:
                    fo_b = None
                    for i_ in range(n):
                        o_ = pe + 24 + optsz + 40 * i_
                        vs2, va2, rs2, ra2 = struct.unpack_from('<4I', A, o_ + 8)
                        if va2 <= buf - base < va2 + rs2: fo_b = ra2 + buf - base - va2
                    rb = exe_bytes(R, x, size)
                    if fo_b is not None and rb == A[fo_b:fo_b + size]:
                        y = amap(x)
                        ob = exe_bytes(O, y, size) if y else None
                        if ob:
                            B[fo_b:fo_b + size] = ob
                            mc['memcmp: rewritten' if ob != rb else 'memcmp: same in ours'] += 1
                        else:
                            mc['memcmp: address not mapped'] += 1
                    else:
                        mc['memcmp: does not hold on retail'] += 1
            if t != ADDR_OF and t != 0x6c9bf6b0:
                pending_x = None
            args = {}
        elif ins.mnemonic in ('ret', 'jmp'):
            pending_x = None; args = {}
    if not got:
        off += 1
st.update(mc)
# `sbb r, r; and r, D; add r, F` picks F or F + D (two functions): D is a distance, which differs in our build
sel = collections.Counter()
off = 0; prev = None
while off < len(code):
    got = False
    for ins in md.disasm(code[off:], base + va + off):
        got = True
        off = ins.address - base - va + ins.size
        ops = ins.operands
        if prev is not None and ins.mnemonic == 'add' and len(ops) == 2 and ops[0].type == X86_OP_REG \
                and ops[1].type == X86_OP_IMM and ops[0].reg == prev[0]:
            F_ = ops[1].imm & 0xffffffff; D_ = prev[1]; G_ = (F_ + D_) & 0xffffffff
            if 0x401000 <= F_ < 0xbd7000 and 0x401000 <= G_ < 0xbd7000:
                f2, g2 = amap(F_), amap(G_)
                if f2 and g2:
                    struct.pack_into('<I', B, prev[2], (g2 - f2) & 0xffffffff)
                    sel['selector distance rewritten'] += 1
                else:
                    sel['selector distance not mapped'] += 1
        if ins.mnemonic == 'and' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM \
                and ins.imm_size == 4:
            prev = (ops[0].reg, ops[1].imm & 0xffffffff, ra + ins.address - base - va + ins.imm_offset)
        else:
            prev = None if ins.mnemonic != 'add' else prev
    if not got:
        off += 1
st.update(sel)
# Signature tables (the action input integration's): 24-byte records {address, 16 bytes, length} in .rdata, the
# game's bytes at the address compared with memcmp. A record counts when its bytes equal the retail exe's there.
rsec = None
for i in range(n):
    o = pe + 24 + optsz + 40 * i
    if A[o:o + 8].rstrip(b'\0') == b'.rdata':
        vs_, va_, rs_, ra_ = struct.unpack_from('<4I', A, o + 8); rsec = (va_, vs_, ra_)
tabs = collections.Counter()
if rsec:
    va_, vs_, ra_ = rsec
    for k in range(0, vs_ - 24, 4):
        x, = struct.unpack_from('<I', A, ra_ + k)
        size, = struct.unpack_from('<I', A, ra_ + k + 20)
        if not (0x401000 <= x < 0xbd7000 and 1 <= size <= 16):
            continue
        want = A[ra_ + k + 4:ra_ + k + 4 + size]
        rb = exe_bytes(R, x, size)
        if rb != want:
            continue
        y = amap(x)
        ob = exe_bytes(O, y, size) if y else None
        if ob is None:
            tabs['table: address not mapped'] += 1
            if os.environ.get('SIGFIX_LOG'): print('  table record not mapped: %08x' % x)
            continue
        tabs['table: same in ours' if ob == rb else 'table: rewritten'] += 1
        # always written: the address pass may have taken pattern bytes for an address and changed them
        B[ra_ + k + 4:ra_ + k + 4 + size] = ob
        if ob != rb:
            fixes.append(('table', hex(x), rb.hex(), ob.hex()))
st.update(tabs)
# SecuROM's `push <return>; jmp <target>` sites are plain CALLs in our build: the plugin's jump-patch builder
# (0x6c9c2620, which checks for an E9 there) becomes its call-patch builder (0x6c9c2590: same arguments, checks
# for E8). A pushed return and a jump is what a CALL does, so the hook behaves the same.
JMP_PATCH, CALL_PATCH = 0x6c9c2620, 0x6c9c2590
# our function starts (a vtable slot of ours holding one is trusted for a pointer patch's expectation)
_ft = os.path.join(os.path.dirname(os.path.abspath(ours_p)), '..', '..', 'port-source-carbon', 'analysis', 'decompiled', 'nfs.exe', 'functions.tsv')
_ft = os.environ.get('FUNCS_TSV', _ft)
FUNC_STARTS = set(int(l.split('\t')[0], 16) for l in open(_ft) if not l.startswith('address')) if os.path.exists(_ft) else set()
def _consensus():
    votes = collections.defaultdict(collections.Counter)
    off_ = 0; site_ = orig_ = None
    while off_ < len(code):
        got_ = False
        for ins in md.disasm(code[off_:], base + va + off_):
            got_ = True; off_ = ins.address - base - va + ins.size; ops_ = ins.operands
            if ins.mnemonic == 'mov' and len(ops_) == 2 and ops_[0].type == X86_OP_MEM and ops_[0].mem.base == X86_REG_ESP \
                    and ops_[1].type == X86_OP_IMM:
                if ops_[0].mem.disp == 4: site_ = ops_[1].imm & 0xffffffff
                if ops_[0].mem.disp == 8: orig_ = ops_[1].imm & 0xffffffff
            if ins.mnemonic == 'call' and ops_ and ops_[0].type == X86_OP_IMM and \
                    (ops_[0].imm & 0xffffffff) in (JMP_PATCH, CALL_PATCH) and site_ and orig_:
                y = amap(site_)
                ob = exe_bytes(O, y, 5) if y else None
                if ob and ob[0] in (0xe8, 0xe9):
                    votes[orig_][(y + 5 + struct.unpack_from('<i', ob, 1)[0]) & 0xffffffff] += 1
        if not got_:
            off_ += 1
    return {o: c.most_common(1)[0][0] for o, c in votes.items() if c.most_common(1)[0][1] >= 2}
consensus = _consensus()
# what the call sites agree on, for the next pass of the map (tools/police_career.py)
json.dump({str(k): v for k, v in consensus.items() if amap(k) != v}, open(os.path.join(WORK, 'consensus.json'), 'w'))
kinds = collections.Counter()
off = 0
last_site = None
last_orig = None
last_table = None
while off < len(code):
    got = False
    for ins in md.disasm(code[off:], base + va + off):
        got = True
        off = ins.address - base - va + ins.size
        ops = ins.operands
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_ESP \
                and ops[0].mem.disp == 4 and ops[1].type == X86_OP_IMM:
            last_site = ops[1].imm & 0xffffffff
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_ESP \
                and ops[0].mem.disp == 8 and ops[1].type == X86_OP_IMM:
            last_orig = (ops[1].imm & 0xffffffff, ra + ins.address - base - va + ins.size - 4)
        if ins.mnemonic == 'call' and ops and ops[0].type == X86_OP_IMM and \
                (ops[0].imm & 0xffffffff) in (0x6c9c2550, 0x6c9c25e0) and last_site and last_orig:
            # a pointer patch (a vtable slot): the slot is mapped; what our slot holds is the function the plugin
            # expects there when it is the same code (or the map has nothing better)
            y = amap(last_site)
            v = struct.unpack_from('<I', O, y - 0x400000)[0] if y and 0x401000 <= y and y - 0x400000 + 4 <= len(O) else None
            e = amap(last_orig[0])
            starts = FUNC_STARTS
            if v and v != e and 0x401000 <= v < 0xbd6000 and (e is None or exe_bytes(O, v, 8) == exe_bytes(R, last_orig[0], 8)
                                                         or (0xbd6000 <= y < 0xd20000 and v in starts)):
                struct.pack_into('<I', B, last_orig[1], v)
                kinds['pointer patch: expected -> what our slot holds'] += 1
        if ins.mnemonic == 'call' and ops and ops[0].type == X86_OP_IMM and \
                (ops[0].imm & 0xffffffff) in (JMP_PATCH, CALL_PATCH) and last_site and last_orig:
            # the callee the patch expects to find there: in our build the CALL/JMP may reach an identical twin of
            # it (functions folded by the linker); then the expectation is our real target
            y = amap(last_site)
            ob = exe_bytes(O, y, 5) if y else None
            if ob and ob[0] in (0xe8, 0xe9):
                real = (y + 5 + struct.unpack_from('<i', ob, 1)[0]) & 0xffffffff
                want_o = amap(last_orig[0])
                if real != want_o and (want_o is None or exe_bytes(O, real, 8) == exe_bytes(R, last_orig[0], 8)
                                       or consensus.get(last_orig[0]) == real):
                    struct.pack_into('<I', B, last_orig[1], real)
                    kinds['expected callee -> its twin in ours'] += 1
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM \
                and 0x6ca00000 <= (ops[1].imm & 0xffffffff) < 0x6ca10000:
            last_table = ops[1].imm & 0xffffffff
        if ins.mnemonic == 'call' and ops and ops[0].type == X86_OP_IMM and (ops[0].imm & 0xffffffff) == JMP_PATCH \
                and last_site is None and last_table:
            # sites from a table (a loop over [table]): every one a CALL in our build -> the call-patch builder
            to = ra_t = None
            for i_ in range(n):
                o_ = pe + 24 + optsz + 40 * i_
                if A[o_:o_ + 8].rstrip(b'\0') == b'.rdata':
                    vs2, va2, rs2, ra2 = struct.unpack_from('<4I', A, o_ + 8)
            fo_t = ra2 + last_table - base - va2
            sites = []
            for k_ in range(16):
                v_ = struct.unpack_from('<I', A, fo_t + 4 * k_)[0]
                if not (0x401000 <= v_ < 0x3080000): break
                sites.append(v_)
            ys = [amap(v_) for v_ in sites]
            if sites and all(y_ and exe_bytes(O, y_, 1) == b'\xe8' for y_ in ys):
                fo = ra + ins.address - base - va
                struct.pack_into('<i', B, fo + 1, CALL_PATCH - (ins.address + 5))
                kinds['jump patch over a table -> call patch'] += 1
        if ins.mnemonic == 'call' and ops and ops[0].type == X86_OP_IMM and (ops[0].imm & 0xffffffff) == JMP_PATCH \
                and last_site is not None:
            y = amap(last_site) if last_site else None
            ob = exe_bytes(O, y, 1) if y else None
            if ob == b'\xe8':
                fo = ra + ins.address - base - va
                struct.pack_into('<i', B, fo + 1, CALL_PATCH - (ins.address + 5))
                kinds['jump patch -> call patch'] += 1
            else:
                kinds['jump patch kept'] += 1
        if ins.mnemonic == 'call':
            last_site = None; last_orig = None
    if not got:
        off += 1
st.update(kinds)
open(adapted_p, 'wb').write(B)
print('signatures:', dict(st))
json.dump(fixes, open(os.path.join(WORK, 'sigfix.json'), 'w'), indent=0)
