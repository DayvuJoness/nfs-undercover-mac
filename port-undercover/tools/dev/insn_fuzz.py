"""Check the translator on every SSE / x87 instruction form a game's listings use, against Unicorn.

    .venv/bin/python -I tools/dev/insn_fuzz.py [--exe original/nfs.exe] [--listings analysis/decompiled/nfs.exe]
                                               [--per-form 3] [--runs 30] [--only MNEM,...] [--x87]

For each distinct form (mnemonic + operand kinds) up to --per-form real instructions are taken from the listing
with their bytes from the exe, each followed by RET, and translated the way a game function is.  Both engines start
from the same random registers, XMM registers (floats or doubles to suit the mnemonic), x87 stack and memory; every
general register, the compared flags, the XMM registers, the x87 stack (to a relative 1e-12) and the touched memory
must agree.  Nothing from the game is written anywhere: the cases live in a temporary folder.  Run it from the port's
root with the kit's virtualenv; clang must be on PATH."""

import argparse
import collections
import ctypes as C
import glob
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import capstone
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn.x86_const import *

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KIT = os.path.join(ROOT, "kit")
sys.path.insert(0, os.path.join(KIT, "tools/recomp"))
sys.path.insert(0, os.path.join(KIT, "tools/recomp/tests"))
import translate as T  # noqa: E402
import test_translate_insns as H  # noqa: E402

CODE0 = 0x0D100000          # case i lives at CODE0 + i * 0x40
SCR = H.SCRATCH             # 64 KB scratch; memory operands point at SCR + 0x8000
FPU_DATA = SCR + 0xF000     # the x87 inputs and outputs
ESP0 = 0x0EF80000          # the stack pointer the cases start with (random data above it)
CW = 0x027F                 # 53-bit precision, round to nearest, as the game runs (its CW is 0x023F/0x027F)
SKIP_MNEM = re.compile(r"^(J|CALL|RET|LOOP|PUSH|POP|ENTER|LEAVE|INT|REP|LOCK|CPUID|RDTSC|FSAVE|FRSTOR|FNSAVE|"
                       r"FLDENV|FNSTENV|FSTENV|FXSAVE|FXRSTOR|LDMXCSR|STMXCSR|FINIT|FNINIT|FLDCW|FNSTCW|FSTCW|"
                       r"FNSTSW|FSTSW|FWAIT|WAIT|PREFETCH|MOVS|STOS|LODS|SCAS|CMPS|XLAT|HLT|FNCLEX|FCLEX)")
MEM = re.compile(r"\[([^\]]*)\]")
XMMR = re.compile(r"\bXMM([0-7])\b")
GPR = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]


def load_pe(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    so = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 52)[0]
    secs = [struct.unpack_from("<8sIIII", d, so + 40 * i) for i in range(n)]

    def read(va, k):
        for _, vs, v, rs, ro in secs:
            if base + v <= va < base + v + rs:
                o = ro + va - base - v
                return d[o:o + k]
        return None
    return read


def form_of(text):
    mnem, _, ops = text.partition(" ")
    kinds = []
    for op in [o.strip() for o in ops.split(",")] if ops else []:
        if "[" in op:
            kinds.append(op.split(" ptr")[0] if " ptr" in op else "mem")
        elif XMMR.match(op):
            kinds.append("xmm")
        elif re.match(r"^MM[0-7]$", op):
            kinds.append("mm")
        elif re.match(r"^ST\d?$", op):
            kinds.append("st")
        elif re.match(r"^(0x)?[0-9a-fA-F]+$", op):
            kinds.append("imm")
        else:
            kinds.append("r" + str(len(op)))
    return mnem + " " + ",".join(kinds)


def collect(listings, exe_read, per_form, want_x87, only, lo=0, hi=0xFFFFFFFF, everything=False):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    forms = collections.OrderedDict()
    for path in sorted(glob.glob(os.path.join(listings, "functions", "*.asm"))):
        for line in open(path, errors="replace"):
            m = re.match(r"^([0-9a-f]{8})  (\S.*)$", line.rstrip("\n"))
            if not m:
                continue
            addr, text = int(m.group(1), 16), m.group(2)
            if not lo <= addr < hi:
                continue
            mnem = text.split(" ")[0]
            if only and mnem not in only:
                continue
            is_sse = "XMM" in text
            is_x87 = mnem.startswith("F")
            if not (everything or is_sse or (want_x87 and is_x87)) or SKIP_MNEM.match(mnem) or "FS:" in text:
                continue
            if "ESP" in text and re.search(r"\[ESP\]", text):
                continue                      # would overwrite the return address
            key = form_of(text)
            lst = forms.setdefault(key, [])
            if len(lst) >= per_form or any(t == text for _, t, _ in lst):
                continue
            raw = exe_read(addr, 16)
            if raw is None:
                continue
            insn = next(md.disasm(raw, addr), None)
            if insn is None:
                continue
            lst.append((addr, text, bytes(raw[:insn.size])))
    return forms


def is_double(mnem):
    return mnem.endswith("PD") or mnem.endswith("SD") or mnem in ("CVTPD2PS", "CVTSD2SS", "CVTTPD2DQ", "CVTPD2DQ")


def rand_float(rng):
    r = rng.random()
    if r < 0.03:
        return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5])
    if r < 0.06:
        return rng.uniform(-1e-3, 1e-3)
    if r < 0.5:
        return rng.uniform(-4.0, 4.0)
    return rng.uniform(-5000.0, 5000.0)


class Case(object):
    def __init__(self, idx, addr, text, code):
        self.idx = idx
        self.orig = addr
        self.addr = CODE0 + 0x40 + idx * 0x40
        self.text = text
        self.code = code + b"\xc3"
        self.name = "%08x %s" % (addr, text)
        self.mnem = text.split(" ")[0]
        self.x87 = self.mnem.startswith("F")
        self.abs_pages = set()
        for e in MEM.findall(text):
            terms = [t.strip() for t in e.split("+")]
            bases = [t for t in terms if t in GPR]
            absd = [int(t, 16) for t in terms if re.match(r"^0x[0-9a-fA-F]+$", t)]
            if not bases and absd:
                a = absd[0]
                self.abs_pages.update({a & ~0xFFF, (a + 0x40) & ~0xFFF})

    def listing(self):
        # The instruction's own relative fields are not position dependent for these forms (no branches), so the
        # listing text is used as is at the new address.
        return "%08x  %s\n%08x  RET\n" % (self.addr, self.text, self.addr + len(self.code) - 1)


class Img(object):
    base = end = size = 0
    md = None

    def __init__(self, cases):
        self.bytes = {}
        for c in cases:
            for i, b in enumerate(c.code):
                self.bytes[c.addr + i] = b

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return None

    def rd8(self, va):
        return self.bytes.get(va)

    def is_exec(self, va):
        return False

    def relocated_pointers(self):
        return set()


def translate_all(cases):
    img = Img(cases)
    tr = T.Translator(img, {c.addr for c in cases}, H.Opts())
    ok, bad = [], {}
    parts = ['#include "x86.h"', ""]
    for c in cases:
        try:
            insns = T.parse_listing_text(c.listing())
            fn = T.Function(c.addr, "f%d" % c.idx, len(c.code), insns)
            fn.measure(img)
            tr.prepare(fn)
            text = "\n".join(tr.translate(fn, ()))
            if "FN(" in text:
                raise T.TranslateError("calls out")
            parts.append("/* %s */\n%s\n" % (c.name, text))
            ok.append(c)
        except Exception as e:  # noqa: BLE001 - every failure is reported, the rest still run
            bad[c.name] = "%s: %s" % (type(e).__name__, e)
    return ok, bad, "\n".join(parts)


def build(ok, csrc):
    work = tempfile.mkdtemp(prefix="insn-fuzz-")
    open(os.path.join(work, "synth.c"), "w").write(csrc)
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        for c in ok:
            fh.write("void fn_%08x(X86 *c);\n" % c.addr)
        fh.write("int recomp_is_call_return(uint32_t t) { return t == 0x%08xu; }\n" % H.MAGIC_RET)
        fh.write("int32_t recomp_index_of(uint32_t t) { switch (t) {\n")
        for i, c in enumerate(ok):
            fh.write("case 0x%08xu: return %d;\n" % (c.addr, i))
        fh.write("default: return -1; } }\n")
        fh.write("void recomp_call(X86 *c, uint32_t t) {\n switch (t) {\n")
        for c in ok:
            fh.write(" case 0x%08xu: fn_%08x(c); return;\n" % (c.addr, c.addr))
        fh.write(" default: recomp_unknown_call(c, t); c->eip = rd32(c->r[R_ESP]); c->r[R_ESP] += 4;\n }\n}\n")
        fh.write("void recomp_jump(X86 *c, uint32_t t) {\n if (t == 0x%08xu) { c->eip = t; return; }\n"
                 " recomp_call(c, t);\n}\n" % H.MAGIC_RET)
    lib = os.path.join(work, "libfuzz.dylib")
    subprocess.check_call(["clang", os.environ.get("FUZZ_OPT", "-O1"), "-g", "-std=c11", "-w", "-fPIC", "-shared", "-I", os.path.join(KIT, "runtime"),
                           os.path.join(KIT, "tools/recomp/tests/harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(work, "table.c"), "-o", lib])
    return H.Native(lib), work


def ext80_to_float(b):
    mant, se = struct.unpack("<QH", b)
    exp = se & 0x7FFF
    sign = -1.0 if se & 0x8000 else 1.0
    if exp == 0 and mant == 0:
        return sign * 0.0
    if exp == 0x7FFF:
        return float("nan") if mant << 1 & ((1 << 64) - 1) else sign * float("inf")
    try:
        return sign * math.ldexp(mant, exp - 16383 - 63)
    except OverflowError:
        return sign * float("inf")


def run_case(case, native, rng):
    """One random run; returns a list of problems."""
    u = Uc(UC_ARCH_X86, UC_MODE_32)
    u.mem_map(H.CODE_BASE, H.CODE_SIZE)
    u.mem_map(CODE0 & ~0xFFFF, 0x100000)
    u.mem_map(H.STACK_BASE, H.STACK_SIZE)
    u.mem_map(SCR, H.SCRATCH_SIZE)
    u.mem_map(H.CAVE, H.CAVE_SIZE)
    u.mem_write(case.addr, case.code)
    mem_writes = []
    # Scratch and the stack above ESP hold random floats / doubles, the same in both engines.
    dbl = is_double(case.mnem)
    blob = b"".join(struct.pack("<d", rand_float(rng)) if dbl else struct.pack("<f", rand_float(rng))
                    for _ in range(0x10000 // (8 if dbl else 4)))
    native.write(SCR, blob)
    u.mem_write(SCR, blob)
    native.zero(H.STACK_BASE, H.STACK_SIZE)
    sblob = bytes(blob[:0x1000])
    u.mem_write(ESP0 + 4, sblob)
    native.write(ESP0 + 4, sblob)
    frame = struct.pack("<I", H.MAGIC_RET)
    u.mem_write(ESP0, frame)
    native.write(ESP0, frame)
    for p in sorted(case.abs_pages):
        try:
            u.mem_map(p, 0x1000)
        except UcError:
            pass
        page = bytes(blob[0x1000:0x2000])
        u.mem_write(p, page)
        native.write(p, page)
        mem_writes.append(p)

    # Registers: every base/index of a memory operand points into scratch (index small).
    regs = {n: rng.getrandbits(32) for n in GPR}
    for e in MEM.findall(case.text):
        terms = [t.strip() for t in e.replace("-", "+-").split("+")]
        regs_in = [t for t in terms if t.split("*")[0].strip() in GPR]
        disp = sum(int(t.replace("-", ""), 16) * (-1 if t.startswith("-") else 1)
                   for t in terms if re.match(r"^-?0x[0-9a-fA-F]+$", t))
        for t in regs_in:
            r = t.split("*")[0].strip()
            if r == "ESP":
                continue
            if "*" in t or t is not regs_in[0]:
                regs[r] = rng.randrange(0, 8)
            else:
                regs[r] = (SCR + 0x8000 - disp) & 0xFFFFFFFF
    regs["ESP"] = ESP0
    # Small integers in the registers a conversion reads, so CVTSI2SS & co see ordinary values.
    c = H.X86()
    for i, n in enumerate(GPR):
        c.r[i] = regs[n]
        u.reg_write(H.UC_REGS[i], regs[n])
    c.eflags_misc = H.EFLAGS_MISC_INIT
    u.reg_write(UC_X86_REG_EFLAGS, H.EFLAGS_MISC_INIT)
    u.reg_write(UC_X86_REG_MXCSR, 0x1F80)
    xmm_ids = [UC_X86_REG_XMM0 + i for i in range(8)]
    for i in range(8):
        if dbl:
            v = struct.pack("<2d", rand_float(rng), rand_float(rng))
        elif case.mnem.startswith("P") or "DQ" in case.mnem and not case.mnem.startswith("CVT"):
            v = bytes(rng.getrandbits(8) for _ in range(16))
        else:
            v = struct.pack("<4f", *(rand_float(rng) for _ in range(4)))
        words = struct.unpack("<4I", v)
        for k in range(4):
            c.xmm[i][k] = words[k]
        u.reg_write(xmm_ids[i], int.from_bytes(v, "little"))
    # x87: three values on the stack (loaded the same way in both engines: pushed through memory).
    c.fpu_cw = CW
    u.reg_write(UC_X86_REG_FPCW, CW)
    c.fpu_tag = H.FPU_TAG_INIT
    # Condition bits left over from earlier code (FSIN once kept a stale C2, 2026-11-15).
    cc0 = rng.choice([0, 0x0100, 0x0400, 0x4000, 0x4500, 0x0600])
    c.fpu_sw = cc0
    u.reg_write(UC_X86_REG_FPSW, cc0)
    if case.x87:
        vals = [rand_float(rng) for _ in range(3)]
        for v in vals:  # FLD qword [FPU_DATA] for each, via a tiny prologue run in Unicorn
            pass
        pro = b"".join(b"\xdd\x05" + struct.pack("<I", FPU_DATA + 8 * k) for k in range(3))
        u.mem_write(FPU_DATA, struct.pack("<3d", *vals))
        u.mem_write(case.addr - 0x20, pro + b"\x90" * (0x20 - len(pro)))
        u.emu_start(case.addr - 0x20, case.addr - 0x20 + len(pro))
        # Native: fpush the same three.
        for v in vals:
            c.fpu_top = (c.fpu_top - 1) & 7
            c.st[c.fpu_top] = v
            c.st_exact[c.fpu_top] = 0
            c.fpu_tag &= ~(3 << (2 * c.fpu_top))
        native.write(FPU_DATA, struct.pack("<3d", *vals))
    try:
        u.emu_start(case.addr, H.MAGIC_RET, count=1000)
    except UcError as e:
        return ["unicorn: %s" % e]
    native.lib.harness_run(case.addr, C.byref(c))

    problems = []
    for i, n in enumerate(GPR):
        uv = u.reg_read(H.UC_REGS[i])
        if c.r[i] != uv:
            problems.append("%s native %08x unicorn %08x" % (n, c.r[i], uv))
    nf = native.lib.harness_eflags(C.byref(c)) & H.EFLAGS_COMPARED
    uf = u.reg_read(UC_X86_REG_EFLAGS) & H.EFLAGS_COMPARED
    if nf != uf:
        problems.append("EFLAGS native %03x unicorn %03x" % (nf, uf))
    for i in range(8):
        uv = u.reg_read(xmm_ids[i])
        nv = sum(c.xmm[i][k] << (32 * k) for k in range(4))
        if uv != nv:
            problems.append("XMM%d native %032x unicorn %032x" % (i, nv, uv))
    for base, size, what in ((SCR, H.SCRATCH_SIZE, "scratch"), (H.STACK_BASE, H.STACK_SIZE, "stack")):
        nb, ub = native.read(base, size), bytes(u.mem_read(base, size))
        if nb != ub:
            k = next(j for j in range(size) if nb[j] != ub[j])
            k &= ~3
            if case.x87 and base == SCR:
                # x87 stores round from 64-bit precision in the CPU; compare as floating values.
                pass
            problems.append("%s at %08x native %s unicorn %s" % (what, base + k, nb[k:k + 8].hex(), ub[k:k + 8].hex()))
    for p in mem_writes:
        nb, ub = native.read(p, 0x1000), bytes(u.mem_read(p, 0x1000))
        if nb != ub:
            problems.append("page %08x differs" % p)
    if case.x87:
        problems_before = len(problems)
        top = (u.reg_read(UC_X86_REG_FPSW) >> 11) & 7
        ucc, ncc = u.reg_read(UC_X86_REG_FPSW) & 0x4700, c.fpu_sw & 0x4700
        if ucc != ncc:
            problems.append("FPSW C0-C3 native %04x unicorn %04x" % (ncc, ucc))
        if top != c.fpu_top:
            problems.append("x87 top native %d unicorn %d" % (c.fpu_top, top))
        else:
            for k in range(3):
                uv = u.reg_read(UC_X86_REG_FP0 + ((k + top) & 7) - 0)  # physical register
                # unicorn returns (mantissa, exponent) for FPn
                if isinstance(uv, tuple):
                    m, ex = uv
                    uf_ = ext80_to_float(struct.pack("<QH", m, ex))
                else:
                    uf_ = float("nan")
                nf_ = c.st[(c.fpu_top + k) & 7]
                if not (math.isnan(uf_) and math.isnan(nf_)) and abs(uf_ - nf_) > 1e-12 * max(1.0, abs(uf_)):
                    problems.append("ST%d native %r unicorn %r" % (k, nf_, uf_))
        if len(problems) > problems_before:
            problems.append("inputs ST0..2 = %r" % (vals[::-1],))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "original/nfs.exe"))
    ap.add_argument("--listings", default=os.path.join(ROOT, "analysis/decompiled/nfs.exe"))
    ap.add_argument("--per-form", type=int, default=3)
    ap.add_argument("--runs", type=int, default=30)
    ap.add_argument("--only", default="")
    ap.add_argument("--x87", action="store_true")
    ap.add_argument("--range", default="", help="LO-HI (hex): only instructions in this address range")
    ap.add_argument("--all", action="store_true", help="integer instructions too, not only SSE / x87")
    a = ap.parse_args()
    only = set(x for x in a.only.split(",") if x)
    lo, hi = (int(x, 16) for x in a.range.split("-")) if a.range else (0, 0xFFFFFFFF)
    forms = collect(a.listings, load_pe(a.exe), a.per_form, a.x87, only, lo, hi, a.all)
    cases = []
    for key, lst in forms.items():
        for addr, text, code in lst:
            cases.append(Case(len(cases), addr, text, code))
    print("%d forms, %d cases" % (len(forms), len(cases)))
    ok, bad, csrc = translate_all(cases)
    for name, err in bad.items():
        print("TRANSLATE  %s  %s" % (name, err))
    native, work = build(ok, csrc)
    rng = random.Random(1234)
    failed = collections.OrderedDict()
    for case in ok:
        for _ in range(a.runs):
            p = run_case(case, native, rng)
            if p:
                failed[case.name] = p
                break
    for name, p in failed.items():
        print("FAIL  %s\n      %s" % (name, "\n      ".join(p[:4])))
    print("%d cases, %d translate errors, %d failed" % (len(cases), len(bad), len(failed)))
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
