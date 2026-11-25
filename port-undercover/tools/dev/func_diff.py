"""Run one whole translated function of a PE image against Unicorn, with the image's sections mapped in both.

    .venv/bin/python -I tools/dev/func_diff.py --exe rebased/msvcr80.dll --listings analysis/decompiled/msvcr80.dll \\
        --function 1005820e --xmm0 1.6316,0.0489,-0.1 [--trace]

XMM0's low double takes each value in turn (the Intel libm's __libm_sse2_* take and return XMM0); every general
register, XMM register and the stack must agree afterwards.  Calls out of the function are not followed (the
translated side returns at once), so pick inputs that stay inside it.  Nothing is written outside a temporary
folder."""
import argparse
import ctypes as C
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import insn_fuzz as F  # noqa: E402
from insn_fuzz import H, T, Uc, UC_ARCH_X86, UC_MODE_32  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.x86_const import *  # noqa: E402,F401


def sections(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    so = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 52)[0]
    out = []
    for i in range(n):
        _, vs, v, rs, ro = struct.unpack_from("<8sIIII", d, so + 40 * i)
        data = d[ro:ro + min(rs, vs or rs)]
        out.append((base + v, max(vs, rs), data))
    return out


class Fn(object):
    def __init__(self, addr, text, code):
        self.addr, self.listing_text, self.code = addr, text, code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--listings", required=True)
    ap.add_argument("--function", required=True)
    ap.add_argument("--xmm0", default="1.0")
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--range", default="", help="LO-HI: also translate every function starting in this range")
    ap.add_argument("--st0", default="", help="x87 inputs (ST0) instead of XMM0")
    ap.add_argument("--cw", default="027f", help="x87 control word to start with")
    ap.add_argument("--sw", default="0000", help="x87 status word condition bits to start with")
    ap.add_argument("--cdecl", action="store_true", help="with --driver: the double argument on the stack")
    ap.add_argument("--driver", action="store_true", help="enter through FLD float [in]; CALL; FSTP double [out]")
    ap.add_argument("--poke", default="", help="ADDR=DWORD,... written into both engines' memory first")
    a = ap.parse_args()
    addr = int(a.function, 16)
    read = F.load_pe(a.exe)
    if a.range:
        lo, hi = (int(x, 16) for x in a.range.split("-"))
    else:
        lo, hi = addr, addr + 1
    fns = []
    import glob
    for path in sorted(glob.glob(os.path.join(a.listings, "functions", "*.asm"))):
        fa = int(os.path.basename(path)[:8], 16)
        if not (lo <= fa < hi or fa == addr):
            continue
        lines = [l.rstrip("\n") for l in open(path, errors="replace") if re.match(r"^[0-9a-f]{8}  ", l)]
        if lines:
            fns.append((fa, lines))
    DRV, IN, OUT = 0x0D200000, 0x0EF90000, 0x0EF90010
    if a.driver and a.cdecl:
        # SUB ESP,8; FLD float [IN]; FSTP double [ESP]; CALL entry; ADD ESP,8; FSTP double [OUT]; RET
        call_at = DRV + 3 + 6 + 3
        rel = addr - (call_at + 5)
        drv_code = (b"\x83\xec\x08" + b"\xd9\x05" + struct.pack("<I", IN) + b"\xdd\x1c\x24" + b"\xe8" +
                    struct.pack("<i", rel) + b"\x83\xc4\x08" + b"\xdd\x1d" + struct.pack("<I", OUT) + b"\xc3")
        fns.append((DRV, ["%08x  SUB ESP,0x8" % DRV, "%08x  FLD float ptr [0x%08x]" % (DRV + 3, IN),
                          "%08x  FSTP double ptr [ESP]" % (DRV + 9), "%08x  CALL 0x%08x" % (call_at, addr),
                          "%08x  ADD ESP,0x8" % (call_at + 5), "%08x  FSTP double ptr [0x%08x]" % (call_at + 8, OUT),
                          "%08x  RET" % (call_at + 14)]))
        real_read = read

        def read(va, k, _r=real_read):  # noqa: F811
            if DRV <= va < DRV + 0x100:
                return (drv_code + bytes(0x100))[va - DRV:va - DRV + k]
            return _r(va, k)
    elif a.driver:
        rel = addr - (DRV + 6 + 5)
        drv_code = (b"\xd9\x05" + struct.pack("<I", IN) + b"\xe8" + struct.pack("<i", rel) +
                    b"\xdd\x1d" + struct.pack("<I", OUT) + b"\xc3")
        fns.append((DRV, ["%08x  FLD float ptr [0x%08x]" % (DRV, IN), "%08x  CALL 0x%08x" % (DRV + 6, addr),
                          "%08x  FSTP double ptr [0x%08x]" % (DRV + 11, OUT), "%08x  RET" % (DRV + 17)]))
        real_read = read

        def read(va, k, _r=real_read):  # noqa: F811
            if DRV <= va < DRV + 0x100:
                return (drv_code + bytes(0x100))[va - DRV:va - DRV + k]
            return _r(va, k)
    lo_all = min(f for f, _ in fns if f != DRV)
    hi_all = max(int(ls[-1][:8], 16) for f, ls in fns if f != DRV) + 16

    class Img(object):
        base = end = size = 0
        md = None

        def insn_end(self, va, mnem):
            return None

        def rd32(self, va):
            b = read(va, 4)
            return struct.unpack("<I", b)[0] if b else None

        def rd8(self, va):
            b = read(va, 1)
            return b[0] if b else None

        def is_exec(self, va):
            return lo_all <= va < hi_all or DRV <= va < DRV + 0x20

        def relocated_pointers(self):
            return set()

    img = Img()
    tr = T.Translator(img, {f for f, _ in fns}, H.Opts())
    parts = ['#include "x86.h"', "#define CALL_FN(a) recomp_call(c, 0x##a##u)"]
    built_fns = []
    for fa, lines in fns:
        try:
            insns = T.parse_listing_text("\n".join(lines) + "\n")
            size = int(lines[-1][:8], 16) + 16 - fa
            fn = T.Function(fa, "f%08x" % fa, size, insns)
            fn.measure(img)
            tr.prepare(fn)
            parts.append("\n".join(tr.translate(fn, ())))
            built_fns.append(fa)
        except Exception as e:  # noqa: BLE001
            print("skip %08x: %s" % (fa, e))
    csrc = "\n".join(parts)
    if os.environ.get("DUMP_C"):
        open(os.environ["DUMP_C"], "w").write(csrc)

    class Ok(object):
        def __init__(self, a_):
            self.addr = a_
    oks = [Ok(f) for f in built_fns]
    native, work = F.build(oks, csrc)
    secs = sections(a.exe)
    use_st0 = bool(a.st0)
    cw = int(a.cw, 16)
    for v in [float(x) for x in (a.st0 or a.xmm0).split(",")]:
        u = Uc(UC_ARCH_X86, UC_MODE_32)
        for va, sz, data in secs:
            lo, hi = va & ~0xFFF, (va + sz + 0xFFF) & ~0xFFF
            try:
                u.mem_map(lo, hi - lo)
            except Exception:
                pass
            u.mem_write(va, data)
            native.write(va, data)
        for item in [x for x in a.poke.split(",") if x]:
            pa, pv = (int(t, 16) for t in item.split("="))
            try:
                u.mem_map(pa & ~0xFFF, 0x1000)
            except Exception:
                pass
            u.mem_write(pa, struct.pack("<I", pv))
            native.write(pa, struct.pack("<I", pv))
        u.mem_map(H.STACK_BASE, H.STACK_SIZE)
        u.mem_map(H.CAVE, H.CAVE_SIZE)
        native.zero(H.STACK_BASE, H.STACK_SIZE)
        esp = 0x0EF80000
        u.mem_write(esp, struct.pack("<I", H.MAGIC_RET))
        native.write(esp, struct.pack("<I", H.MAGIC_RET))
        c = H.X86()
        for i in range(8):
            val = esp if i == 4 else 0x1000 * i
            c.r[i] = val
            u.reg_write(H.UC_REGS[i], val)
        import random as _r
        rr = _r.Random(int(abs(v) * 1e6))
        for k in range(1, 8):
            words = [rr.getrandbits(32) for _ in range(4)]
            for j in range(4):
                c.xmm[k][j] = words[j]
            u.reg_write(UC_X86_REG_XMM0 + k, sum(w << (32 * j) for j, w in enumerate(words)))
        hi = [rr.getrandbits(32), rr.getrandbits(32)]
        c.xmm[0][2], c.xmm[0][3] = hi
        x = struct.unpack("<2I", struct.pack("<d", v))
        c.xmm[0][0], c.xmm[0][1] = x
        u.reg_write(UC_X86_REG_XMM0, struct.unpack("<Q", struct.pack("<d", v))[0] | (hi[0] << 64) | (hi[1] << 96))
        c.eflags_misc = H.EFLAGS_MISC_INIT
        u.reg_write(UC_X86_REG_EFLAGS, H.EFLAGS_MISC_INIT)
        u.reg_write(UC_X86_REG_MXCSR, 0x1F80)
        c.fpu_cw = cw
        u.reg_write(UC_X86_REG_FPCW, cw)
        c.fpu_tag = H.FPU_TAG_INIT
        sw0 = int(a.sw, 16) & 0x4700
        c.fpu_sw = sw0
        u.reg_write(UC_X86_REG_FPSW, sw0)
        if a.driver:
            u.mem_map(DRV, 0x1000)
            u.mem_write(DRV, drv_code)
            u.mem_write(IN, struct.pack("<f", v))
            native.write(IN, struct.pack("<f", v))
        elif use_st0:
            u.mem_write(0x0EF90000, struct.pack("<d", v))
            u.mem_write(H.CAVE + 0x100, b"\xdd\x05" + struct.pack("<I", 0x0EF90000))
            u.emu_start(H.CAVE + 0x100, H.CAVE + 0x106)
            c.fpu_top = 7
            c.st[7] = v
            c.fpu_tag &= ~(3 << 14)
        if a.trace:
            def tr_(uc, ad, sz, user):
                xs = " ".join("%016x" % (uc.reg_read(UC_X86_REG_XMM0 + k) & ((1 << 64) - 1)) for k in range(8))
                print("U %08x eax=%08x edx=%08x %s" % (ad, uc.reg_read(UC_X86_REG_EAX), uc.reg_read(UC_X86_REG_EDX), xs))
            u.hook_add(UC_HOOK_CODE, tr_)
        entry = DRV if a.driver else addr
        u.emu_start(entry, H.MAGIC_RET, count=100000)
        native.lib.harness_run(entry, C.byref(c))
        if a.driver:
            uo = struct.unpack("<d", bytes(u.mem_read(OUT, 8)))[0]
            no = struct.unpack("<d", native.read(OUT, 8))[0]
            print("x=%r driver: unicorn %r translated %r %s" % (v, uo, no, "OK" if uo == no else "DIFFER"))
        un = struct.unpack("<d", struct.pack("<Q", u.reg_read(UC_X86_REG_XMM0) & ((1 << 64) - 1)))[0]
        nn = struct.unpack("<d", struct.pack("<2I", c.xmm[0][0], c.xmm[0][1]))[0]
        if use_st0:
            top = (u.reg_read(UC_X86_REG_FPSW) >> 11) & 7
            m, ex = u.reg_read(UC_X86_REG_FP0 + top)
            un = F.ext80_to_float(struct.pack("<QH", m, ex))
            nn = c.st[c.fpu_top]
            print("   top unicorn %d native %d, cw unicorn %04x native %04x" % (top, c.fpu_top,
                  u.reg_read(UC_X86_REG_FPCW), c.fpu_cw))
        print("x=%r  unicorn %r  translated %r  %s" % (v, un, nn, "OK" if un == nn else "DIFFER"))
        for i in range(8):
            uv = u.reg_read(UC_X86_REG_XMM0 + i)
            nv = sum(c.xmm[i][k] << (32 * k) for k in range(4))
            if uv != nv:
                print("   XMM%d native %032x unicorn %032x" % (i, nv, uv))
        for i, n in enumerate(F.GPR):
            if c.r[i] != u.reg_read(H.UC_REGS[i]):
                print("   %s native %08x unicorn %08x" % (n, c.r[i], u.reg_read(H.UC_REGS[i])))


if __name__ == "__main__":
    main()
