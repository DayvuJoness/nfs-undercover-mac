#!/usr/bin/env python3
"""Make the Police Career plugin run on this port's nfs.exe (2026-11-19).

    .venv/bin/python tools/police_career.py [--mods dist/mods_to_add] [--game <game folder>]

Police Career 1.0.2 is made for the retail 46.6 MB nfs.exe 1.0.0.1 (SecuROM); this port is made from the
10.5 MB 1.0.0.1, a separately linked build of the same source. This writes rebased/PoliceCareer.asi, a copy of
the person's plugin whose references to the retail exe point at the same code and data in ours:
  1. every one of our functions is found in the retail exe by its bytes (addresses masked), and the absolute
     operands of matched instructions pair the two builds' data (tools/dev/asi/exemap.py);
  2. the plugin's references are read operand by operand (refs.py); code addresses the first pass missed are
     matched back (revmatch2.py) or by their slot between matched neighbours (slotmatch.py) - SecuROM moved the
     bodies of some retail functions into its own section;
  3. the references are rewritten (adapt.py), the byte signatures it checks game code against before hooking it
     are rewritten to our bytes where they held on the retail exe (sigfix.py), its msvcrt.dll imports point at the game's msvcr80.dll, and the
     copy is relocated to the base game.toml gives it ([modules.aux.c_policecareer]).
Nothing of the plugin or of either exe goes into the repository: rebased/ and build/ are not part of it.
The retail nfs.exe the plugin ships with is only read, for the matching.
"""
import argparse, hashlib, json, os, re, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from rebase_pe import rebase  # noqa: E402

BASE = 0x10130000
# SecuROM jump slots the plugin hooks (2026-11-19). In the retail exe two game functions jump through a SecuROM
# pointer cell (0x005718e0: `push ebx; jmp [0x01d991fc]`; 0x0051e290: `jmp [0x01da1b24]`); the plugin hooks them by
# repointing the instruction's cell (A) or the cell's content (B). Ours are plain functions (0x00571b80, 0x0051df10),
# so the plugin is given cells in our image (game.toml data_seeds 0x01375840..) that hold our function, and the kit
# runs the plugin's routine when it changes them (RECOMP_SLOT_HOOKS, launch.c).
SLOT_OVERRIDES = {
    0x005718e3: 0x01375840,  # A: the JMP's memory operand -> our cell pointer
    0x01d991fc: 0x01375844,  # A: the cell
    0x01defe60: 0x00571b80,  # A: what the cell holds: the function (the kit undoes its `push ebx` on the way back)
    0x01da1b24: 0x01375850,  # B: the cell
    0x01e14be0: 0x0051df10,  # B: what it holds: the function itself
    0x0051e290: 0x0051df10,  # B: the function
    # The career/garage object (2026-11-20): retail keeps its car-record list (0x14a rows of 0x1c) at +8, ours at
    # +0x88 (every field from 8 on is 0x80 further: 0x317e8/0x4d56c/0x4d6ac -> 0x31868/0x4d5ec/0x4d72c, measured
    # on both exes' `mov ecx,[global]; add ecx,N`). The plugin only reaches it through this global and only at +8,
    # so it reads a cell core.nfsu keeps at [0x00d8e1a4] + 0x80 (police_career_pass): its +8 is our list.
    0x00da1f24: 0x01375870,
}
# Text the plugin draws (UTF-16 in its .rdata), shortened where it doesn't fit our screen (2026-11-20): the
# police cars menu's big title runs off the left edge in Russian (the game's own title there is "Машины").
TEXT_PATCHES = [
    ("Полицейские машины", "Полиция"),
]
KEY = "c_policecareer"
PY = sys.executable


def rename_import(data, old, new):
    """Point the import descriptor naming `old` at a new name placed in .rdata's slack."""
    data = bytearray(data)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        nm = data[o:o + 8].rstrip(b"\0").decode()
        vs, va, rs, ra = struct.unpack_from("<4I", data, o + 8)
        secs.append([nm, o, vs, va, rs, ra])

    def off(rva):
        for nm, o, vs, va, rs, ra in secs:
            if va <= rva < va + rs:
                return ra + rva - va
        raise ValueError(hex(rva))

    imp_rva = struct.unpack_from("<I", data, opt + 96 + 8)[0]
    rd = next(s for s in secs if s[0] == ".rdata")
    nm, o, vs, va, rs, ra = rd
    slot = (vs + 15) & ~15
    if slot + len(new) + 1 > rs:
        raise SystemExit(".rdata has no room for the new import name")
    data[ra + slot:ra + slot + len(new) + 1] = new.encode() + b"\0"
    struct.pack_into("<I", data, o + 8, slot + 16)  # the section now maps the name too
    n = 0
    i = 0
    while True:
        d = off(imp_rva) + 20 * i
        name_rva = struct.unpack_from("<I", data, d + 12)[0]
        if not name_rva:
            break
        e = data.index(b"\0", off(name_rva))
        if data[off(name_rva):e].decode().lower() == old.lower():
            struct.pack_into("<I", data, d + 12, va + slot)
            n += 1
        i += 1
    if not n:
        raise SystemExit("no %s import descriptor" % old)
    return bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mods", type=Path, default=ROOT / "dist/mods_to_add")
    ap.add_argument("--game", type=Path, default=ROOT / "original")
    a = ap.parse_args()
    mod = next((p for p in a.mods.iterdir() if p.is_dir() and (p / "SCRIPTS/PoliceCareer.asi").is_file()), None)
    if mod is None:
        sys.exit("no Police Career folder (SCRIPTS/PoliceCareer.asi) in %s" % a.mods)
    asi, retail, ours = mod / "SCRIPTS/PoliceCareer.asi", mod / "nfs.exe", a.game / "nfs.exe"
    work = ROOT / "build/police_career"
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, WORK=str(work), OURS_EXE=str(ours), RETAIL_EXE=str(retail),
               FUNCS_TSV=str(ROOT / "analysis/decompiled/nfs.exe/functions.tsv"))
    tools = ROOT / "tools/dev/asi"
    funcs = ROOT / "analysis/decompiled/nfs.exe/functions.tsv"

    def run(*args):
        subprocess.run([PY, "-I"] + [str(x) for x in args], check=True, env=env)

    overrides = dict(SLOT_OVERRIDES)
    for attempt in range(3):
        json.dump({str(k): v for k, v in overrides.items()}, open(work / "overrides.json", "w"))
        if not pipeline(run, tools, ours, retail, funcs, asi, work):
            break
        cons = {int(k): v for k, v in json.load(open(work / "consensus.json")).items()}
        cf = work / "callfix.json"
        if cf.exists():
            cons.update({int(k): v for k, v in json.load(open(cf)).items()})
        new = {k: v for k, v in cons.items() if overrides.get(k) != v}
        if not new:
            break
        print("call sites agree on %d targets the map had otherwise; another pass" % len(new))
        overrides.update(new)
    finish(asi, work, funcs)


def pipeline(run, tools, ours, retail, funcs, asi, work):
    run(tools / "exemap.py", ours, retail, funcs, work / "map.json")
    run(tools / "vtmatch.py", ours, retail, funcs, work / "vt.json")
    run(tools / "refs.py", asi, work / "refs.json")
    # code values the forward match left out
    sys.path.insert(0, str(tools))
    m = json.load(open(work / "map.json"))
    import bisect
    F = sorted((r, x, s) for x, r, s in m["functions"])
    FR = [f[0] for f in F]

    def code_map(x):
        i = bisect.bisect_right(FR, x) - 1
        return F[i][1] + x - F[i][0] if i >= 0 and F[i][0] <= x < F[i][0] + F[i][2] else None

    refs = json.load(open(work / "refs.json"))
    want = sorted(set(v for _, v, _, _ in refs if v < 0xBD7000 and code_map(v) is None))
    json.dump(want, open(work / "want.json", "w"))
    run(tools / "revmatch2.py", ours, retail, work / "want.json", work / "rev2.json")
    run(tools / "slotmatch.py", ours, retail, funcs, work / "want.json", work / "rev2.json", work / "slot.json")
    run(tools / "secmatch.py", ours, retail, work / "refs.json", work / "securom.json")
    run(tools / "adapt.py", asi, work / "adapted.asi")
    # its byte signatures of game code, rewritten where our build's bytes differ (sigfix.py)
    run(tools / "sigfix.py", asi, work / "adapted.asi", retail, ours)
    return True


def patch_texts(data):
    data = bytearray(data)
    for old, new in TEXT_PATCHES:
        a, b = (old + "\0").encode("utf-16-le"), new.encode("utf-16-le")
        hits = [m.start() for m in re.finditer(re.escape(a), bytes(data)) if m.start() % 2 == 0]
        if len(hits) != 1 or len(b) > len(a) - 2:
            sys.exit("text patch %r: %d places (want exactly 1)" % (old, len(hits)))
        data[hits[0]:hits[0] + len(a)] = b + b"\0" * (len(a) - len(b))
        print("text %r -> %r" % (old, new))
    return bytes(data)


def finish(asi, work, funcs):
    data = rename_import(patch_texts((work / "adapted.asi").read_bytes()), "msvcrt.dll", "msvcr80.dll")
    data, old, size, n = rebase(data, BASE)
    out = ROOT / "rebased"
    out.mkdir(exist_ok=True)
    (out / "PoliceCareer.asi").write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()
    print("PoliceCareer.asi %#010x -> %#010x  size %#x  %d relocations  sha256 %s" % (old, BASE, size, n, digest))
    toml = ROOT / "game.toml"
    text = toml.read_text()
    sec = re.search(r"(?ms)^\[modules\.aux\.%s\]\n.*?(?=^\[|\Z)" % KEY, text)
    if sec:
        body = re.sub(r'(?m)^sha256 = "[0-9a-f]*"', 'sha256 = "%s"' % digest, sec.group(0))
        body = re.sub(r"(?m)^size = 0x[0-9a-fA-F]+", "size = %#x" % size, body)
        toml.write_text(text[:sec.start()] + body + text[sec.end():])
        print("game.toml [modules.aux.%s] updated" % KEY)


if __name__ == "__main__":
    main()
