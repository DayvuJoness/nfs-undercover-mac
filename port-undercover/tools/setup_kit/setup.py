#!/usr/bin/env python3
"""The setup kit: turn your own installed copy of Need for Speed: Undercover (PC) into the native Mac app.
Run through ./setup.sh, which prepares the Python environment first.

    ./setup.sh "/path/to/Need for Speed - Undercover" [--out DIR] [--button-icons] [--hd-ui] [--check-only]

Steps: check the game folder, nfs.exe and its runtime DLLs -> link the game (nothing in it is changed) ->
relocate the runtime DLLs -> analyse nfs.exe once (Ghidra and a Java runtime, pinned hashes), adapt the built-in
Police Career plugin to it and analyse that -> translate and build -> check the built-in mods (dist/mods_to_add,
put there by the Setup app) -> package "Need for Speed Undercover.app" with them. Everything
else stays on this Mac. The Setup app (tools/installer/) runs this and reads its "n/7" step lines."""
import argparse
import hashlib
import platform
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

ROOT = Path(__file__).resolve().parents[2]
LOG = ROOT / "build/setup.log"
APP_NAME = "Need for Speed Undercover"
PROFILE = "NFS Undercover"
# The runtime DLLs as the game installs them: game.toml's addresses in them (data_ranges, entry_points) are for
# these exact files, relocated by tools/rebase_pe.py.
DLLS = {
    "msvcr80.dll": "02bdde38e4c6bd795a092d496b8d6060cdbe71e22ef4d7a204e3050c1be44fa9",
    "msvcp80.dll": "2026f3c4f830dff6883b88e2647272a52a132f25eb42c0d423e36b3f65a94d08",
}
PY = sys.executable
# The mods the app is built with (Setup app, 2026-11-25): the Setup app puts them here with a list of every file's
# SHA-256 beside them (MODS_DIR + ".sha256"); package.py and tools/police_career.py read them from here.
MODS_DIR = "dist/mods_to_add"
PC_KEY = "c_policecareer"
# The adapted Police Career of the tested build (the app in the person's project/mac folder, 2026-11-25).
PC_TESTED_SHA256 = "5b27595328e639ce83a6e3c1003f17012e83bfa597ef4ff3865543a74cb84e99"
# The environment's own tools (cmake, ninja from kit/requirements-dev.txt) come first on PATH for every step.
ENV = None


def say(msg):
    print(msg, flush=True)
    with LOG.open("a") as f:
        f.write(msg + "\n")


def fail(msg):
    say("\nSETUP STOPPED: " + msg)
    sys.exit(1)


def run(args, **kw):
    say("  $ " + " ".join(str(a) for a in args))
    with LOG.open("a") as log:
        kw.setdefault("cwd", ROOT)
        kw.setdefault("env", ENV)
        r = subprocess.run([str(a) for a in args], stdout=log, stderr=subprocess.STDOUT, **kw)
    if r.returncode:
        fail(f"step failed (exit {r.returncode}); the last lines of {LOG} say why")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find(game, name):
    """game/name without regard to case (Windows game folders mix cases)."""
    return next((p for p in game.iterdir() if p.name.lower() == name.lower()), None)


def find_path(game, rel):
    """game/rel ("GLOBAL/X.BUN") without regard to case, or None."""
    p = game
    for part in rel.split("/"):
        p = find(p, part) if p is not None and p.is_dir() else None
    return p


def check_game(game, cfg):
    if not game.is_dir():
        fail(f"{game} is not a folder")
    name = cfg["game"]["executable"]
    exe = find(game, name)
    if exe is None:
        fail(f"no {name} in {game}; choose the installed game folder (the one with {name}, GLOBAL, TRACKS ...)")
    names = {p.name.upper() for p in game.iterdir() if p.is_dir()}
    missing = [d for d in cfg["setup"]["required_dirs"] if d not in names]
    if missing:
        fail(f"the game folder is incomplete: {', '.join(missing)} missing")
    # Files the game opens at start-up and cannot do without (game.toml [setup] required_files; case-insensitive).
    absent = [f for f in cfg["setup"].get("required_files", []) if find_path(game, f) is None]
    if absent:
        fail("the game folder is incomplete: " + ", ".join(absent) + " missing. Copy the whole installed game "
             "folder again from the disc or the original installation; every one of these files is part of a "
             "full install.")
    if sha(exe) != cfg["game"]["sha256"]:
        fail(f"This {name} can't be used. The port is built for one exact nfs.exe: the PC game's last version, "
             "without SecuROM disc protection (SHA-256 " + cfg["game"]["sha256"][:12] + "...). Nothing was changed.")
    say(f"  {name}: OK")
    for name, digest in DLLS.items():
        p = find(game, name)
        if p is None:
            fail(f"{name} is missing from the game folder (the game installs it beside nfs.exe)")
        if sha(p) != digest:
            fail(f"This {name} is a different version from the one the game installs; put the game's own copy "
                 "back beside nfs.exe. Nothing was changed.")
    say("  " + ", ".join(DLLS) + ": OK")


def link_game(game):
    """original -> the game folder. The game folder itself is never written to."""
    dest = ROOT / "original"
    if dest.is_symlink():
        dest.unlink()
    elif dest.exists():
        fail(f"{dest} is a real folder, not a link; move it aside")
    dest.symlink_to(game, target_is_directory=True)
    say(f"  linked {game}")


def check_mods(mods):
    """Every built-in mod file is there and unchanged (the list the Setup app put beside them)."""
    listing = Path(str(mods) + ".sha256")
    if not mods.is_dir() or not listing.is_file():
        fail(f"the built-in mods are missing ({mods}); download the Setup app again")
    rows = [line.split("  ", 1) for line in listing.read_text().splitlines() if line.strip()]
    bad = [rel for digest, rel in rows if not (mods / rel).is_file() or sha(mods / rel) != digest]
    if bad:
        fail(f"{len(bad)} built-in mod files are missing or changed (first: {bad[0]}); download the Setup app again")
    names = sorted({rel.split("/", 1)[0] for _, rel in rows})
    say(f"  {len(rows)} files of {len(names)} mods: OK")
    for name in names:
        say(f"    {name}")


def analysis_done(cfg):
    out = ROOT / "analysis/decompiled"
    return all((out / name / "functions.tsv").is_file() for name in (cfg["game"]["executable"], *DLLS))


def main():
    global ENV
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path, help="your installed game folder (holds nfs.exe, GLOBAL, TRACKS, ...)")
    ap.add_argument("--out", type=Path, default=ROOT / "dist", help="where to put the app (default dist/)")
    ap.add_argument("--hd-ui", action="store_true",
                    help="HD menu textures (Carbon only; accepted and skipped for Undercover)")
    ap.add_argument("--button-icons", action="store_true",
                    help="controller button icons (Carbon only; accepted and skipped for Undercover)")
    ap.add_argument("--check-only", action="store_true", help="only check the game folder")
    ap.add_argument("--ghidra-home", type=Path, help="use this Ghidra 12.1.3 instead of downloading one")
    ap.add_argument("--java-home", type=Path, help="use this Java runtime (21 or later) instead of downloading one")
    args = ap.parse_args()
    if platform.machine() != "arm64":
        fail("this Python runs under Rosetta (Intel). Run ./setup.sh, which picks a native Apple Silicon Python.")
    import os
    ENV = dict(os.environ, PATH=str(Path(PY).parent) + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"))
    (ROOT / "build").mkdir(exist_ok=True)
    LOG.write_text("")
    cfg = tomllib.loads((ROOT / "game.toml").read_text())
    game = args.game.expanduser().resolve()

    say("1/7 Checking your game folder")
    check_game(game, cfg)
    if args.check_only:
        say("Check passed. Nothing was changed.")
        return

    say("2/7 Linking the game")
    link_game(game)

    say("3/7 Preparing the game's runtime DLLs")
    run([PY, "tools/rebase_pe.py", game])

    say("4/7 Analysing nfs.exe (once; can take a while)")
    tools = {}

    def ghidra_tools():
        if not tools:
            sys.path.insert(0, str(ROOT / "tools"))
            import fetch_tools
            java = args.java_home or fetch_tools.fetch("jdk")
            ghidra = args.ghidra_home or fetch_tools.fetch("ghidra")
            fetch_tools.build_decompiler(ghidra, java)
            tools.update(java=java, ghidra=ghidra)
        return ["--ghidra-home", tools["ghidra"], "--java-home", tools["java"]]

    # nfs.exe and its runtime DLLs first: the Police Career plugin is adapted from nfs.exe's listing.
    if analysis_done(cfg):
        say("  already analysed; reused")
    else:
        run([PY, "tools/analyze.py", *ghidra_tools(), "--only", cfg["game"]["executable"].lower(),
             *[n.lower() for n in DLLS]])
    # Police Career (built in, Setup app 2026-11-25): the person's plugin made for the retail exe, rewritten to run
    # on this nfs.exe by tools/police_career.py, then analysed like the DLLs.
    mods = ROOT / MODS_DIR
    if not mods.is_dir():
        fail(f"the built-in mods are missing ({mods}); download the Setup app again")
    asi = ROOT / "rebased/PoliceCareer.asi"
    if asi.is_file() and sha(asi) == cfg["modules"]["aux"][PC_KEY]["sha256"]:
        say("  Police Career: already adapted; reused")
    else:
        say("  Police Career: adapting the built-in plugin to your nfs.exe")
        run([PY, "tools/police_career.py", "--mods", mods, "--game", game])
        cfg = tomllib.loads((ROOT / "game.toml").read_text())
    digest = cfg["modules"]["aux"][PC_KEY]["sha256"]
    say("  Police Career: " + ("the same plugin as the tested build" if digest == PC_TESTED_SHA256
        else "adapted (it differs from the tested build's, SHA-256 " + digest[:12] + "...)"))
    listing = ROOT / "analysis/decompiled/PoliceCareer.asi"
    inputs = listing / "inputs.json"
    import json
    done = (listing / "functions.tsv").is_file() and inputs.is_file() and \
        json.loads(inputs.read_text()).get("executable_sha256") == digest
    if done:
        say("  Police Career: already analysed; reused")
    else:
        run([PY, "tools/analyze.py", *ghidra_tools(), "--only", "policecareer.asi"])

    say("5/7 Translating and building")
    run([PY, "tools/build.py", "--regenerate"])
    binary = cfg["game"]["app_name"]
    if not (ROOT / f"build/{binary}.app/Contents/MacOS/{binary}").is_file():
        fail(f"the build finished without build/{binary}.app")

    say("6/7 Checking the built-in mods")
    check_mods(mods)

    say("7/7 Packaging")
    out = args.out.expanduser().resolve()
    package = [PY, "tools/setup_kit/package.py", "--game", game, "--out", out, "--mods", mods]
    run(package)
    say(f"\nDone: {out / (APP_NAME + '.app')}\nYour saves and settings will be in "
        f"~/Library/Application Support/{PROFILE}/.")


if __name__ == "__main__":
    main()
