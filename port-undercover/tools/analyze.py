#!/usr/bin/env python3
"""Export this game's translation inputs with Ghidra's own analysis.

    tools/analyze.py --ghidra-home /path/to/ghidra_12.1.3_PUBLIC [--java-home ...]

The kit's tools/setup.py imports a curated annotation set into Ghidra and
exports with analysis switched off, which suits a game whose functions were
recovered by hand. No such set exists for speed.exe, so this script imports
the linked executable, runs Ghidra's default analyzers, and exports the same
listing layout the kit's translator reads (functions.tsv, functions/*.asm)
into ignored analysis/decompiled/nfs.exe with the kit's ExportProgram.java.
It then does the same for each [modules.aux.*] DLL (the rebased msvcr80.dll
and msvcp80.dll from tools/rebase_pe.py). Run tools/setup.py --install ...
--link-only and tools/rebase_pe.py first. nfs.exe's 7.4 MB .text section
takes a long time."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
GHIDRA_VERSION = "12.1.3"


def load_game_config():
    spec = importlib.util.spec_from_file_location("game_config", KIT / "tools/game_config.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra-home", type=Path, default=os.environ.get("GHIDRA_HOME"))
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--max-memory", default="8G", help="Ghidra's Java heap (default 8G)")
    parser.add_argument("--only", nargs="*", type=str.lower, help="analyse only these files, e.g. msvcr80.dll")
    args = parser.parse_args()
    if not (KIT / "tools/ExportProgram.java").is_file():
        sys.exit("The kit folder is missing: clone the whole repository again")
    if not args.ghidra_home:
        sys.exit("Set --ghidra-home or GHIDRA_HOME to the extracted Ghidra %s directory" % GHIDRA_VERSION)
    ghidra = args.ghidra_home.expanduser().resolve()
    properties = ghidra / "Ghidra/application.properties"
    if not properties.is_file() or "application.version=%s\n" % GHIDRA_VERSION not in properties.read_text():
        sys.exit("Use Ghidra %s; set --ghidra-home to its extracted directory" % GHIDRA_VERSION)
    cfg = load_game_config().load(ROOT)
    exe = cfg["developer_exe_path"]
    if not exe.is_file():
        sys.exit("%s is missing: run tools/setup.py --install ... --link-only first" % exe)
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    if digest != cfg["game"]["sha256"]:
        sys.exit("Unsupported %s: SHA-256 %s; expected %s" % (exe.name, digest, cfg["game"]["sha256"]))
    env = dict(os.environ)
    if args.java_home:
        env["JAVA_HOME"] = str(args.java_home.expanduser().resolve())
        env["PATH"] = str(Path(env["JAVA_HOME"]) / "bin") + os.pathsep + env.get("PATH", "")
    env["MAXMEM"] = args.max_memory
    # Functions Ghidra never makes by itself (reached only through pointers): made before its analysis runs,
    # so their bodies and switch tables are in the listing (tools/ghidra/CreateEntryFunctions.java).
    images = [(exe, cfg["listings_path"], cfg["game"]["sha256"], cfg["translate"].get("entry_points", []))]
    only = set(args.only or [])
    for module in cfg.get("aux_modules", []):
        # Setup app (2026-11-25): a fresh setup analyses nfs.exe and its DLLs first (--only), because
        # tools/police_career.py needs nfs.exe's listing to make rebased/PoliceCareer.asi; a module left out by
        # --only need not exist yet.
        if only and module["path"].name.lower() not in only:
            continue
        if not module["path"].is_file():
            sys.exit("%s is missing: run tools/rebase_pe.py first" % module["path"])
        digest = hashlib.sha256(module["path"].read_bytes()).hexdigest()
        if digest != module["sha256"]:
            sys.exit("%s: SHA-256 %s; game.toml expects %s (rerun tools/rebase_pe.py)"
                     % (module["path"], digest, module["sha256"]))
        images.append((module["path"], module["listings_path"], module["sha256"], module["entry_points"]))
    only = set(args.only or [])
    for image, listings, sha, entries in images:
        if only and image.name.lower() not in only:
            continue
        output = listings.parent
        project = output.parent / "ghidra"
        project.mkdir(parents=True, exist_ok=True)
        output.mkdir(parents=True, exist_ok=True)
        # The exporter writes one file per function and never removes any, so a listing from an earlier
        # analysis (another build of the exe, or functions Ghidra no longer makes) would stay beside the new one
        # and be translated as if it were current. 2026-11-14: after the switch from 1.1.2.1 to 1.0.0.1, stale
        # 1.1.2.1 bodies (00597920 reading the old cookie address 0x01272378) were translated into the new build
        # and the game failed its own stack check. Each image's listing folder starts empty.
        if listings.is_dir():
            shutil.rmtree(listings)
        print("Analysing %s" % image.name, flush=True)
        command = [
            str(ghidra / "support/analyzeHeadless"), str(project), image.name,
            "-import", str(image), "-deleteProject",
            "-scriptPath", str(KIT / "tools") + ";" + str(ROOT / "tools/ghidra"),
        ]
        if entries:
            command += ["-preScript", "CreateEntryFunctions.java"] + ["%#x" % a for a in entries]
        command += ["-postScript", "ExportProgram.java", str(output)]
        subprocess.run(command, cwd=ROOT, env=env, check=True)
        index = listings / "functions.tsv"
        if not index.is_file() or len(index.read_text().splitlines()) < 2:
            sys.exit("Ghidra did not export a function index for %s; inspect its output" % image.name)
        (listings / "inputs.json").write_text(json.dumps({
            "executable_sha256": sha,
            "annotations_revision": None,
            "ghidra_analysis": "default analyzers",
            "entry_functions": ["%08x" % a for a in entries],
            "ghidra_version": GHIDRA_VERSION,
        }, indent=2) + "\n")
    print("Listings ready in analysis/decompiled. Next: tools/build.py --regenerate")


if __name__ == "__main__":
    main()
