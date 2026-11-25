#!/usr/bin/env python3
"""Package the built game with your game files: "Need for Speed Undercover.app".

    python tools/setup_kit/package.py --game "/path/to/Need for Speed - Undercover"
    python tools/setup_kit/package.py --game DIR --install     # also put it in /Applications

Takes build/UndercoverRecomp.app (run tools/build.py first) and adds:
  Contents/MacOS/Launch           tools/setup_kit/launch.c, the app's executable
  Contents/Resources/Game/        your game folder (APFS clones, so almost no extra disk
                                  space), minus Windows-only files, plus the rebased
                                  msvcr80/msvcp80 DLLs the translation was made from
  Contents/Resources/Defaults/    first-run registry.json and mod-settings.json
  Contents/Resources/AppIcon.icns the game's own logo from your FRONTEND files (game_art.py)
Info.plist gets the app's name, the racing-games category and LSSupportsGameMode, so macOS
turns on Game Mode in full screen. Every Mach-O is signed ad hoc, inside out.

Nothing here is game data: the app it writes holds YOUR copy and is not for sharing.
"""
import argparse
import fnmatch
import hashlib
import json
import plistlib
import shutil
import subprocess
import sys
try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = Path(__file__).resolve().parent
APP_NAME = "Need for Speed Undercover"
VERSION = "0.1.0"
MAGIC = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
# Beyond game.toml's [bundle] exclude: the installer and online-only leftovers, and what other tools keep in
# the game folder (this port's own source folder when it sits there, Craft Anything's records).
EXTRA_EXCLUDE = ["setup.exe", "server.cfg", "autorun.inf", "*.ico", "port-source-*", "*.json", "*.jsonl", "*.log",
                 "*.cany", "checks", "capture", "inbox", "reference", "attachments", "pinned"]
def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def is_macho(p):
    with open(p, "rb") as f:
        return f.read(4) in MAGIC


def find_exe(game, name):
    for p in game.iterdir():
        if p.name.lower() == name.lower():
            return p
    return None


def clone_tree(src, dst, exclude, exe_name):
    """Copy the game folder with APFS clones (cp -c) where possible. This repository is skipped when it
    sits inside the game folder."""
    dst.mkdir(parents=True)
    for p in sorted(src.iterdir()):
        name = p.name.lower()
        if name.startswith(".") or name == exe_name.lower() or any(fnmatch.fnmatch(name, x) for x in exclude):
            continue
        if p.resolve() == ROOT or ROOT.is_relative_to(p.resolve()):
            continue
        if subprocess.run(["/bin/cp", "-cR", str(p), str(dst / p.name)], capture_output=True).returncode:
            subprocess.run(["/bin/cp", "-R", str(p), str(dst / p.name)], check=True)


def sign(app):
    nested = sorted((q for q in app.rglob("*") if q.is_file() and not q.is_symlink() and is_macho(q)
                     and "Resources/Game" not in str(q)),
                    key=lambda q: (-len(q.relative_to(app).parts), str(q)))
    for q in nested:
        subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(q)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(app)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)


# The registry Language names nfs.exe knows (its table at 0x00cd8b10, 12-byte rows {name, language id, ...},
# read by 007318d0 from HKLM\Software\EA Games\Need for Speed Undercover "Language"; "Engish" is the game's own
# spelling), by the code in the speech data file SOUND/SPEECH/copspeechdat_<code>.big. Only "ru" and "en" have been
# seen in a real install (the person's: copspeechdat_ru.big, no copspeechdat_en.big); the other codes are the
# usual EA ones, not yet seen.
SPEECH_LANGUAGES = {"en": "Engish (US)", "ru": "Russian", "fr": "French", "de": "German", "it": "Italian",
                    "es": "Spanish", "sv": "Swedish", "da": "Danish", "cs": "Czech", "hu": "Hungarian",
                    "nl": "Dutch", "pl": "Polish", "fi": "Finnish"}


def game_language(game):
    """The Language registry name for the speech the install carries, else None (the game's default).

    Without a Language value the game asks for the English cop speech (Sound\\Speech\\copspeechdat_en.big) even in
    a Russian install that has only copspeechdat_ru.big; the bank stays unread (the game's 0xAA fill) and the
    main thread faulted in FUN_0095a4e0 at 0095a53b (2026-11-14). So the one speech language present is chosen."""
    speech = next((p for p in game.iterdir() if p.name.lower() == "sound"), None)
    speech = next((p for p in speech.iterdir() if p.name.lower() == "speech"), None) if speech else None
    if not speech:
        return None
    found = {p.name.lower()[len("copspeechdat_"):-len(".big")] for p in speech.iterdir()
             if p.name.lower().startswith("copspeechdat_") and p.name.lower().endswith(".big")}
    found &= set(SPEECH_LANGUAGES)
    if len(found) != 1 or found == {"en"}:
        return None
    return SPEECH_LANGUAGES[found.pop()]


def set_default_language(registry, language):
    """Write Language (REG_SZ) into the first-run registry.json beside the game's other options."""
    data = json.loads(registry.read_text())
    key = next(iter(data))
    data[key]["Language"] = {"type": 1, "data": language}
    registry.write_text(json.dumps(data, indent=2) + "\n")


# (file in the mods folder, its place under Resources/ModData)
def hud_keep_textures(mod_path, orig_path, names):
    """Put the named textures of a texture pack back as the game's own file has them (picture bytes and info), when
    the mod's pack is the game's changed in place (same size, same texture table). Returns what was done."""
    import struct
    O = orig_path.read_bytes()
    M = bytearray(mod_path.read_bytes())
    if len(O) != len(M):
        return "not the game's pack changed in place - left as the mod has it"
    loc = {}

    def walk(o, end):
        while o + 8 <= end:
            t, sz = struct.unpack_from("<II", O, o)
            loc.setdefault(t, (o, sz))
            if t & 0x80000000:
                walk(o + 8, o + 8 + sz)
            o += 8 + sz
    walk(0, len(O))
    if 0x33310004 not in loc or 0x33320002 not in loc:
        return "no plain texture table - left as the mod has it"
    io, isz = loc[0x33310004]
    do = loc[0x33320002][0]
    kept, p = [], io + 8
    while p < io + 8 + isz:
        n = O[p + 0x58]
        name = O[p + 0x59:p + 0x59 + n].split(b"\0")[0]
        if name in names and M[p:p + 0x59 + n][0x59:].split(b"\0")[0] == name:
            off, size = struct.unpack_from("<I", O, p + 0x14)[0], struct.unpack_from("<I", O, p + 0x1c)[0]
            M[do + 8 + off:do + 8 + off + size] = O[do + 8 + off:do + 8 + off + size]
            M[p:p + 0x59 + n] = O[p:p + 0x59 + n]
            kept.append(name.decode())
        p += 0x59 + n
    mod_path.write_bytes(bytes(M))
    return "the game's own " + (", ".join(kept) if kept else "(none found)") + " kept"


MOD_FILES = [
    ("Need for Speed Undercover Redux/NFSUCRedux.tpf", "redux/NFSUCRedux.tpf"),
    ("Need for Speed Undercover Redux/GLOBAL/InGameA.bun", "redux/InGameA.bun"),
    ("b46f5a-Prostreet Sky Mod/japan3.tpf", "sky/japan3.tpf"),
    ("b46f5a-Prostreet Sky Mod/Sky shit.tpf", "sky/skyfix.tpf"),
    ("Undercover High Contrast HUD/Easy method - No tools required/HUDTEXRACE.BIN", "hchud/HUDTEXRACE.BIN"),
    # VltEd scripts, applied by core.nfsu to the live attribute database (mods/core/nfsu/attrib.inc)
    ("Need for Speed Undercover Redux/UCPO.nfsms", "redux/UCPO.nfsms"),
    ("Revamped Pursuits (UC)/Vanilla/Revamped Pursuits (Cop Perf).nfsms", "pursuits/RevampedPursuits.nfsms"),
    # Hard Drivers (2026-11-20): one script per difficulty, the F10 choice picks one (core.nfsu mod_hard_drivers)
    ("Hard Drivers/Hard Drivers/Easy.nfsms", "harddrivers/Easy.nfsms"),
    ("Hard Drivers/Hard Drivers/Normal.nfsms", "harddrivers/Normal.nfsms"),
    ("Hard Drivers/Hard Drivers/Hard.nfsms", "harddrivers/Hard.nfsms"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", type=Path, required=True, help="your game folder (the one with nfs.exe)")
    ap.add_argument("--out", type=Path, default=ROOT / "dist", help="where to write the app (default dist/)")
    ap.add_argument("--install", action="store_true", help="also copy the app to /Applications")
    ap.add_argument("--icon", type=Path, help="a picture to use as the app icon (default: the game's own icon)")
    ap.add_argument("--mods", type=Path, default=ROOT / "dist/mods_to_add",
                    help="the folder with the game mods the app carries (default dist/mods_to_add; skipped if absent)")
    a = ap.parse_args()

    cfg = tomllib.loads((ROOT / "game.toml").read_text())
    BINARY = cfg["game"]["app_name"]
    exe_name = cfg["game"]["executable"]
    built = ROOT / f"build/{BINARY}.app"
    if not (built / f"Contents/MacOS/{BINARY}").exists():
        sys.exit(f"{built} is missing: run tools/build.py first")
    game = a.game.expanduser().resolve()
    exe = find_exe(game, exe_name) if game.is_dir() else None
    if not exe:
        sys.exit(f"no {exe_name} in {game}")
    if sha256(exe) != cfg["game"]["sha256"]:
        sys.exit(f"{exe} is not the supported {exe_name} (SHA-256 {cfg['game']['sha256']})")
    aux = []
    for key, mod in sorted(cfg.get("modules", {}).get("aux", {}).items()):
        p = ROOT / mod["path"]
        if not p.exists() or sha256(p) != mod["sha256"]:
            sys.exit(f"{p} is missing or not the rebased {mod['name']}: run tools/rebase_pe.py")
        aux.append((mod["name"], p))
    if subprocess.run(["/usr/bin/pgrep", "-f", f"{APP_NAME}.app/Contents/MacOS/{BINARY}"],
                      capture_output=True).returncode == 0:
        sys.exit("the game is running; quit it first")

    a.out.mkdir(parents=True, exist_ok=True)
    app = a.out / f"{APP_NAME}.app"
    tmp = a.out / f".{APP_NAME}.app.new"
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.copytree(built, tmp, symlinks=True)
    shutil.rmtree(tmp / "Contents/_CodeSignature", ignore_errors=True)
    res = tmp / "Contents/Resources"

    launch = tmp / "Contents/MacOS/Launch"
    subprocess.run(["/usr/bin/clang", "-O2", "-Wall", "-mmacosx-version-min=13.0", "-o", str(launch),
                    str(KIT / "launch.c"), "-framework", "CoreFoundation"], check=True)
    (res / "Defaults").mkdir(exist_ok=True)
    for n in ("registry.json", "mod-settings.json"):
        shutil.copyfile(KIT / "defaults" / n, res / "Defaults" / n)
    language = game_language(game)
    if language:
        set_default_language(res / "Defaults/registry.json", language)
        print(f"language: {language} (the only language in your game folder)")
    for n in ("LICENSE", "NOTICE"):
        if (ROOT / n).exists():
            shutil.copyfile(ROOT / n, res / n)

    exclude = [x.lower() for x in cfg.get("bundle", {}).get("exclude", [])] + EXTRA_EXCLUDE
    print(f"copying the game from {game} ...")
    clone_tree(game, res / "Game", exclude, exe_name)
    shutil.copyfile(exe, res / "Game" / exe_name)
    for name, p in aux:
        shutil.copyfile(p, res / "Game" / name)

    # The game mods the F10 menu switches (2026-11-19): their files, from the person's mods folder, go into
    # Resources/ModData under fixed names that launch.c points the kit at. Nothing of them is kept in this repository.
    mods = a.mods.expanduser()
    if mods.is_dir():
        for src, dst in MOD_FILES:
            f = mods / src
            if f.is_file():
                (res / "ModData" / dst).parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(["/bin/cp", "-c", str(f), str(res / "ModData" / dst)], check=True)
                print(f"mod file: {src}")
            else:
                print(f"mod file missing (that mod stays off): {src}")
        # The High Contrast HUD (2026-11-25, the person: "the event prompt shows the pad's d-pad instead of Tab"): the
        # mod was made from the console HUD and its HUDTEXRACE.BIN also replaces HUD_RACE_NOW_PC, the PC's "Tab" key
        # cap over an event, with a d-pad. The pack is changed in place (same size and offsets as the game's), so that
        # one texture's picture and info are taken back from the game's own file; the mod's 22 other textures stay.
        hc = res / "ModData" / "hchud" / "HUDTEXRACE.BIN"
        orig_hud = Path(a.game).expanduser() / "GLOBAL" / "HUDTEXRACE.BIN"
        if hc.is_file() and orig_hud.is_file():
            print("mod file: High Contrast HUD - " + hud_keep_textures(hc, orig_hud, [b"HUD_RACE_NOW_PC"]))
        # Police Career (2026-11-19): its data beside the game as its installer puts it - SCRIPTS\\ (its ini and
        # the gameplay, car, event and UI resources it loads) and UI\\ (the Interceptor maps). The plugin itself is
        # the adapted copy tools/police_career.py made (rebased/, an aux module copied above); its own nfs.exe and
        # dinput8 loader are not used.
        pc = next((p for p in mods.iterdir() if p.is_dir() and (p / "SCRIPTS/PoliceCareer.asi").is_file()), None)
        if pc:
            for sub in ("SCRIPTS", "UI"):
                for f in sorted((pc / sub).iterdir()):
                    if f.is_file() and f.suffix.lower() not in (".asi", ".dll", ".log"):
                        (res / "Game" / sub).mkdir(parents=True, exist_ok=True)
                        subprocess.run(["/bin/cp", "-c", str(f), str(res / "Game" / sub / f.name)], check=True)
            print(f"mod files: Police Career ({pc.name}) SCRIPTS and UI")
        # Add-on cars (2026-11-20): tools/addon_cars.py makes their attribute vault (Game/GLOBAL/ADDONCARS.BIN,
        # mounted by core.nfsu), the GLOBALB bundle with their car types/collisions/parts and the run-time list
        # (ModData/addoncars, read in place of GLOBALB.LZC while the setting is on); their models, damage and
        # engine sounds have new names and go into Game/ as their readmes say; files that replace a game file
        # (the Miura's FRONTEND/LOGOTEXTURES.BUN) go into ModData/addoncars/replace and are read through the overlay.
        ac = res / "ModData" / "addoncars"
        r = subprocess.run([sys.executable, str(ROOT / "tools/addon_cars.py"), "--mods", str(mods), "--game",
                            str(game), "--out", str(ac)], capture_output=True, text=True)
        if r.returncode == 0 and (ac / "files.txt").is_file():
            (res / "Game" / "GLOBAL").mkdir(parents=True, exist_ok=True)
            shutil.move(str(ac / "ADDONCARS.BIN"), str(res / "Game" / "GLOBAL" / "ADDONCARS.BIN"))
            n = 0
            for line in (ac / "files.txt").read_text().splitlines():
                p = line.split("\t")
                if len(p) == 3 and p[0] == "overlay":
                    dst = ac / "replace" / p[2]
                elif len(p) == 2:
                    dst = res / "Game" / p[1]
                else:
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(["/bin/cp", "-c", p[-2], str(dst)], check=True)
                n += 1
            # files the tool made (renamed models, game files with additions) are in Game/ or replace/ now
            for sub in ("converted", "made", "packed"):
                shutil.rmtree(ac / sub, ignore_errors=True)
            print(f"add-on cars: {r.stdout.count('add-on car ')} cars, {n} files")
        elif r.stdout.strip() or r.stderr.strip():
            print("add-on cars left out:", (r.stdout + r.stderr).strip()[-400:])

    info = tmp / "Contents/Info.plist"
    plist = plistlib.loads(info.read_bytes())
    plist.update(CFBundleName=APP_NAME, CFBundleDisplayName=APP_NAME, CFBundleExecutable="Launch",
                 CFBundleIdentifier=cfg["game"]["bundle_id"],
                 CFBundleShortVersionString=VERSION, CFBundleVersion=VERSION,
                 LSApplicationCategoryType="public.app-category.racing-games", LSSupportsGameMode=True,
                 NSHighResolutionCapable=True)
    # The icon: the player's picture, else the game's own logo (from their FRONTEND files), else the icon
    # in their nfs.exe (it has none), else ours.
    sys.path.insert(0, str(KIT))
    import make_icon
    import game_art
    icns = res / "AppIcon.icns"
    try:
        if a.icon:
            make_icon.icns_from_image(a.icon, icns)
            made = True
        else:
            logo = game_art.game_logo(game)
            if logo is not None:
                make_icon.icns_from_logo(logo, icns)
                made = True
            else:
                made = make_icon.icns_from_exe(exe, icns)
    except Exception as e:  # noqa: BLE001 - any failure falls back to the generic icon
        print(f"icon: {e}; using the generic one")
        made = False
    if not made:
        shutil.copyfile(KIT / "defaults/AppIcon.icns", icns)
    plist["CFBundleIconFile"] = "AppIcon"
    info.write_bytes(plistlib.dumps(plist))

    print("signing ...")
    sign(tmp)
    if app.exists():
        shutil.rmtree(app)
    tmp.rename(app)
    print(f"packaged {app}")

    if a.install:
        dest = Path("/Applications") / app.name
        if dest.exists():
            shutil.rmtree(dest)
        subprocess.run(["/usr/bin/ditto", str(app), str(dest)], check=True)
        print(f"installed {dest}")


if __name__ == "__main__":
    main()
