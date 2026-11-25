"""Map addresses the plugin uses inside the retail exe's .securom section (code SecuROM moved out of game
functions) onto ours. A moved block ends with a JMP back into .text, to the instruction that followed the moved code
in the original function; so in our build the same instructions end right before map(that target). Each candidate
is checked byte by byte (rel32 and absolute fields compared through the map).
usage: secmatch.py ours.exe retail.exe refs.json out.json   (WORK: map files)"""
import os, sys, json, struct, bisect
from capstone import *
from capstone.x86 import *
O = open(sys.argv[1], 'rb').read(); R = open(sys.argv[2], 'rb').read()
refs = json.load(open(sys.argv[3]))
exec(open(os.path.join(os.path.dirname(__file__), 'adapt.py')).read().split("pe = struct.unpack_from")[0]
     .replace("asi = bytearray(open(sys.argv[1], 'rb').read())", ""))
SEC_LO, SEC_HI = 0x1d22000, 0x3072000  # .securom
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
def rd(b, a, n):
    o = a - 0x400000
    return b[o:o + n] if 0 <= o and o + n <= len(b) else b''
out = {}
why = {}
def call_form(x):
    # `push <return>; jmp <target>` (SecuROM's CALL): ours has `call map(target)` after the same instructions
    b = rd(R, x - 5, 10)
    if len(b) < 10 or b[0] != 0x68 or b[5] != 0xe9: return None
    ret = struct.unpack_from('<I', b, 1)[0]
    if not (x < ret < x + 0x40): return None
    t = (x + 5 + struct.unpack_from('<i', b, 6)[0]) & 0xffffffff
    tt = amap(t)
    if not tt: return None
    start = x - 5
    best_hits = None
    for back in range(6, 33):
        cand = start - back
        insns = list(md.disasm(rd(R, cand, back), cand))
        if not insns or sum(i.size for i in insns) != back: continue
        # absolute operands inside the prefix are compared through the map
        pre = bytearray(rd(R, cand, back)); fix = []
        for i in insns:
            for fo, fs in ((i.imm_offset, i.imm_size), (i.disp_offset, i.disp_size)):
                if fo and fs == 4:
                    v = struct.unpack_from('<I', bytes(i.bytes), fo)[0]
                    if 0x400000 <= v < SEC_HI:
                        m = amap(v) if v < SEC_LO else None
                        if m is None: break
                        struct.pack_into('<I', pre, i.address - cand + fo, m)
            else:
                continue
            break
        else:
            hits = []
            p = O.find(bytes(pre), 0x1000, 0x7d6000)
            while p >= 0 and len(hits) < 8:
                q = p + len(pre)
                if O[q] == 0xe8 and (q + 0x400000 + 5 + struct.unpack_from('<i', O, q + 1)[0]) & 0xffffffff == tt:
                    hits.append(q + 0x400000)
                p = O.find(bytes(pre), p + 1, 0x7d6000)
            if os.environ.get('SECDBG') and x == int(os.environ['SECDBG'], 16): print('dbg', hex(x), back, [hex(h) for h in hits])
            if len(hits) == 1: return hits[0]
            if not hits:
                if best_hits: hits = best_hits
                else: continue  # decoded from inside an instruction: try a longer run
            best_hits = hits
            if back < 24 and len(hits) > 1:
                continue  # a longer run of the same instructions may single one out
            # several: the one nearest to where the moved block finally returns into the game's code
            back_t = None
            blk = rd(R, x, 0x200)
            starts = set(FR)
            for k_ in range(len(blk) - 6):
                if blk[k_] == 0x68 and blk[k_ + 5] == 0xc3:
                    v_ = struct.unpack_from('<I', blk, k_ + 1)[0]
                    # a return into the middle of a function (not a call made by push/ret)
                    # the push/ret that is not a call (a call's push/ret comes right after another push: its return)
                    prev_ = struct.unpack_from('<I', blk, k_ - 4)[0] if k_ >= 5 and blk[k_ - 5] == 0x68 else None
                    # a call made by push/ret returns into SecuROM's section; the way back into the game does not
                    if 0x401000 <= v_ < 0xbd7000 and not (prev_ and SEC_LO <= prev_ < SEC_HI):
                        back_t = v_; break
            bt = amap(back_t) if back_t else None
            if back_t and not bt:
                # roughly: shifted like the nearest function the forward match paired
                i_ = bisect.bisect_right(FR, back_t) - 1
                if i_ >= 0: bt = back_t + F[i_][1] - F[i_][0]
            # or: the candidate whose neighbourhood calls what the moved block calls (push/ret calls)
            called = set()
            for k_ in range(5, len(blk) - 6):
                if blk[k_] == 0x68 and blk[k_ + 5] == 0xc3:
                    v_ = struct.unpack_from('<I', blk, k_ + 1)[0]
                    m_ = amap(v_) if 0x401000 <= v_ < 0xbd7000 else None
                    if m_: called.add(m_)
            def near_calls(h):
                n_ = 0
                for q in range(h - 0x80 - 0x400000, h + 0x100 - 0x400000):
                    if O[q] == 0xe8 and (q + 0x400000 + 5 + struct.unpack_from('<i', O, q + 1)[0]) & 0xffffffff in called:
                        n_ += 1
                return n_
            if called and not bt:
                sc = sorted(((near_calls(h), -h, h) for h in hits), reverse=True)
                if sc[0][0] > 0 and sc[0][0] > sc[1][0]:
                    return sc[0][2]
                # equal scores: the first of a run of calls is the block's first
                best = [h for c_, _, h in sc if c_ == sc[0][0]]
                if sc[0][0] > 0 and best:
                    return min(best)
            if os.environ.get('SECDBG') and x == int(os.environ['SECDBG'], 16): print('dbg back', hex(back_t or 0), hex(bt or 0))
            if bt:
                hits.sort(key=lambda h: abs(h - bt))
                if abs(hits[0] - bt) < 0x400 and abs(hits[1] - bt) > 2 * abs(hits[0] - bt):
                    return hits[0]
    return None
for x in sorted(set(v for _, v, _, _ in refs if SEC_LO <= v < SEC_HI)):
    cf = call_form(x)
    if cf:
        out[x] = cf
        continue
    if rd(R, x - 5, 1) == b'\x68' and rd(R, x, 1) == b'\xe9':
        why[x] = 'call form not found'; continue
    code = rd(R, x, 512)
    insns = []
    end = None
    for ins in md.disasm(code, x):
        insns.append(ins)
        if ins.mnemonic == 'jmp' and ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm & 0xffffffff
            if 0x401000 <= t < 0xbd7000:
                end = (ins.address, t)
            break
        if ins.mnemonic in ('ret', 'jmp') or len(insns) > 80:
            break
    if not end:
        why[x] = 'no jump back'; continue
    L = end[0] - x
    tt = amap(end[1])
    cands = [tt - L] if tt else []
    if not cands:
        # the target is in a function the matchers missed: look for the block's bytes anywhere in our .text
        mask = bytearray(b'\xff' * L)
        for ins in insns[:-1]:
            rel = ins.address - x
            if (ins.mnemonic == 'call' or ins.mnemonic.startswith('j')) and ins.size >= 5:
                mask[rel + ins.size - 4:rel + ins.size] = b'\0' * 4
            for fo, fs in ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size)):
                if fo and fs == 4 and fo + 4 <= ins.size and 0x400000 <= struct.unpack_from('<I', bytes(ins.bytes), fo)[0] < SEC_HI:
                    mask[rel + fo:rel + fo + 4] = b'\0' * 4
        runs = []; st_ = None
        for k in range(L + 1):
            um = k < L and mask[k] == 0xff
            if um and st_ is None: st_ = k
            elif not um and st_ is not None: runs.append((k - st_, st_)); st_ = None
        if runs:
            ln, s0 = max(runs)
            key = code[s0:s0 + ln]
            if ln >= 6:
                p = O.find(key, 0x1000, 0x7d6000)
                while p >= 0 and len(cands) < 20:
                    cands.append(p - s0 + 0x400000); p = O.find(key, p + 1, 0x7d6000)
    if not cands and len(insns) == 2 and insns[0].mnemonic == 'call' and insns[0].operands[0].type == X86_OP_IMM:
        # just `call X; jmp back`: ours has `call map(X)` there; among our calls to it, the one no other
        # address of the plugin already took
        t0 = insns[0].operands[0].imm & 0xffffffff
        tt0 = amap(t0)
        if tt0:
            taken = set()
            for _, v_, _, _ in refs:
                if v_ != x and v_ < SEC_LO:
                    m_ = amap(v_)
                    if m_: taken.add(m_)
            mine = [q + 0x400000 for q in range(0x1000, 0x7d6000 - 5)
                    if O[q] == 0xe8 and (q + 0x400000 + 5 + struct.unpack_from('<i', O, q + 1)[0]) & 0xffffffff == tt0]
            free = [q for q in mine if q not in taken and (q + 1) not in taken]
            if len(free) == 1:
                cands = free
    if not cands:
        why[x] = 'jump target not mapped'; continue
    good = []
    for cand in cands:
      ob = rd(O, cand, L)
      ok = len(ob) == L
      for ins in (insns[:-1] if ok else []):
        rel = ins.address - x
        raw = bytes(ins.bytes); mine = ob[rel:rel + ins.size]
        fields = {}
        if (ins.mnemonic == 'call' or ins.mnemonic.startswith('j')) and ins.size >= 5 and ins.operands[0].type == X86_OP_IMM:
            fields[ins.size - 4] = 'rel'
        for fo, fs in ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size)):
            if fo and fs == 4 and fo + 4 <= ins.size:
                v = struct.unpack_from('<I', raw, fo)[0]
                if 0x400000 <= v < SEC_HI: fields[fo] = 'abs'
        for k in range(ins.size):
            if any(fo <= k < fo + 4 for fo in fields): continue
            if raw[k] != mine[k]: ok = False
        for fo, kind in fields.items():
            vr = struct.unpack_from('<I', raw, fo)[0]; vo = struct.unpack_from('<I', mine, fo)[0]
            if kind == 'rel':
                vr = (ins.address + ins.size + vr) & 0xffffffff; vo = (cand + rel + ins.size + vo) & 0xffffffff
            e = amap(vr) if vr < SEC_LO else None
            if e is not None and e != vo: ok = False
        if not ok: break
      if ok: good.append(cand)
    if len(good) == 1:
        out[x] = good[0]
    else:
        why[x] = 'bytes differ' if not good else 'ambiguous'
print('securom addresses mapped', len(out), 'of', len(out) + len(why), {k: v for k, v in why.items() if v != 'no jump back'} and '')
json.dump(out, open(sys.argv[4], 'w'))
