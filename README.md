# Need for Speed: Undercover — Native for Apple Silicon Macs (Setup)

**Need for Speed Undercover Setup** builds a native macOS version of *Need for Speed: Undercover* (PC, 2008) from
**your own personal Steam copy of the game**, on your own Mac. The game is not emulated and does not run under Wine:
its x86 code is translated to C ahead of time (static recompilation) and compiled for Apple Silicon, with a Metal
renderer, native sound and controller support, a settings page (F10) and 24 community mods built in.

> **You need your own personal Steam copy of the game** — *Need for Speed: Undercover* bought on **your** Steam
> account and installed by Steam on a Windows PC. Not just any copy: the port is made for the exact `nfs.exe` Steam
> installs (version 1.0.0.1, without SecuROM). A disc copy has a different, protected `nfs.exe` and cannot be used.
>
> This repository and its release contain **no game code and no game files**. The Setup app carries the port's
> source code and the mods; the game itself is built from your `nfs.exe` on your Mac, and nothing is uploaded.

This project is not affiliated with or endorsed by Electronic Arts. *Need for Speed* and *Undercover* are
trademarks of Electronic Arts Inc.

**Developer:** DayvuJoness. **Tools:** Claude (Anthropic) and Craft Anything.
Based on **Need for Speed Carbon Setup**, the installer of the
[Need for Speed: Carbon Mac port](https://github.com/vasist15-source/nfs-carbon-mac) by vasist15-source.

---

## Mac requirements

| | Minimum | Recommended |
|---|---|---|
| Mac | Apple Silicon: M1 or later (MacBook Air / Pro, iMac, Mac mini, Mac Studio). **Intel Macs are not supported.** | M2 Pro / M3 or later |
| macOS | macOS 13 Ventura or later | the latest macOS (tested on macOS 26) |
| Memory | 8 GB | 16 GB or more (the game uses 3.5–4.5 GB while playing) |
| Free disk space | about 20 GB: your copied game folder (~6.2 GB), the setup's tools and build files (~6 GB, in `~/Library/NFSU-Native-Setup`), the finished game (~6.7 GB; much less when the game folder is on the same disk, because files are cloned) | 30 GB |
| Internet | needed for the **first** build only: it downloads its build tools (Python packages, Ghidra 12.1.3 and a Java runtime, about 800 MB) | |
| Apple Command Line Tools | free; the Setup app installs them for you (button **Install…**) | |
| Time | the first build: about 15–60 minutes depending on the Mac (the one-time analysis of `nfs.exe` is the long part); later builds are much quicker | |

Tested on an M4 Pro MacBook Pro with macOS 26: a full first build took about 15 minutes; in-game 100–120 fps at
3024×1964 on a 120 Hz screen.

## How to use

1. **Buy and install the game on Steam on a Windows PC** (your own Steam account).
2. **Copy the whole installed game folder to your Mac** (the folder with `nfs.exe`, `GLOBAL`, `TRACKS`, `CARS`, …;
   usually `C:\Program Files (x86)\Steam\steamapps\common\Need for Speed Undercover`).
3. Download **Need for Speed Undercover Setup** from the [latest release](../../releases/latest), unzip it and open
   it. The app is not notarized by Apple: the first time, open it, click **Done**, then go to
   **System Settings → Privacy & Security** and click **Open Anyway**.
4. Click **Choose…** and pick `nfs.exe` in your copied game folder (or drag it onto the window). The app checks it
   first and changes nothing if it can't be used.
5. If asked, click **Install…** to get Apple's Command Line Tools and finish Apple's installer.
6. Click **Build**. When it finishes, click **Open the game**. The game goes to `/Applications` (or a folder you
   choose).

Your saves and settings are kept in `~/Library/Application Support/NFS Undercover/`. Your game folder is never
written to. The setup's own files in `~/Library/NFSU-Native-Setup/` are safe to delete after building (an update is
quicker with them).

### What the build does (7 steps)

1. **Checks your game folder:** the SHA-256 of `nfs.exe` (`d1ecded0…c7d2e9`), `msvcr80.dll` and `msvcp80.dll`, and
   the folders the game needs.
2. **Links the game** (nothing in it is changed).
3. **Prepares the game's runtime DLLs** (relocated copies).
4. **Analyses `nfs.exe`** with Ghidra (once) and prepares the built-in Police Career plugin.
5. **Translates and builds** the game for Apple Silicon.
6. **Checks the built-in mods** (every file against its SHA-256).
7. **Packages** `Need for Speed Undercover.app` with your game data and the mods.

## In the game

- **F10** (fn+F10 on a Mac keyboard) opens the settings: window mode (full screen, borderless, windowed), frame-rate
  limit and vertical sync, resolution scale and MetalFX, sharpening, motion blur, draw distance, HDR and more, and a
  **Mods** page where every built-in mod can be switched on or off.
- Any Mac resolution, including the notch-free full screen, in the game's own Options → Video.
- Controllers (Xbox, PlayStation, Switch Pro) with rumble.

## Built-in mods and their authors

Thank you to every one of them. Each mod can be switched in the game's F10 menu (Mods page).

| Mod | Author |
|---|---|
| Alfa Romeo Brera | Toshiya Joshima (God Arm) |
| Aston Martin One-77 | Toshiya Joshima (God Arm) |
| Ferrari FXX-K Evoluzione | Toshiya Joshima (God Arm) |
| Hard Drivers | Wurf |
| High Contrast HUD | TheShveyn04 |
| Honda NSX-R GT | YochiThMaster333 |
| Koenigsegg CCXR Special Edition | Toshiya Joshima (God Arm) |
| Koenigsegg Regera | Toshiya Joshima (God Arm) |
| Lada 1200s | YochiThMaster333 |
| Lada Samara | YochiThMaster333 |
| Lamborghini Aventador SVJ | Toshiya Joshima (God Arm) |
| Lamborghini Miura P400 SV | N4SPEED |
| Lexus GS430 '04 | PedroBLR (conversion; model from Drift Spirits) |
| Marussia B2 | LegSolo |
| Mercedes-AMG GT R | Toshiya Joshima (God Arm) |
| Photorealistic (with Extra Options) | Miscellaneous and ExOpts Team |
| Police Career 1.0.2 | kolomei |
| Porsche Pack | Toshiya Joshima (God Arm) |
| ProStreet Sky | LegSolo |
| Revamped Pursuits | Splash45 |
| Subaru WRX STI '04 | YochiThMaster333 |
| Toyota Corolla | LegSolo |
| Toyota Corolla Trueno '83 | PedroBLR (conversion; model from Drift Spirits) |
| Undercover Redux | Miscellaneous |

The mods' own credits also thank nfsu360 (NFS-VltEd, Car Toolkit), MaxHwoy (Binary) and Oleg Melashenko (ZModeler).
The mod files are the work of their authors and are included as they distributed them.

## Credits

- **DayvuJoness** — developer of this Setup app and of the Undercover port.
- **Claude** (Anthropic) and **Craft Anything** — the tools it was made with.
- **vasist15-source** — the [Need for Speed: Carbon Mac port](https://github.com/vasist15-source/nfs-carbon-mac) and
  its installer, *Need for Speed Carbon Setup*, which this Setup app and the Undercover port are based on (MIT).
- **veritr1x** — recomp-kit, the static recompiler and runtime (in `port-undercover/kit/`, MIT).
- The mod authors listed above.

## Contact

Everything goes through this repository's [Issues](../../issues/new/choose) — there is no e-mail address:

- **Thanks and feedback** — say thank you, share how the game runs on your Mac, suggest improvements.
- **Bug reports** — something doesn't build or doesn't work in the game.
- **Removal requests** — if you are the author of a built-in mod or another rights holder and want something taken
  down or credited differently, please open a removal request first; it will be handled promptly.

## Building the Setup app from source

The repository holds the Setup app (Rust, eframe/egui) and, in `port-undercover/`, the Undercover port's source
(no game files). The mods are too large for git (one texture pack is over 100 MB), so take them from the release:

```sh
# 1. the mods: from the released app
cp -R "/Applications/Need for Speed Undercover Setup.app/Contents/Resources/mods_to_add" payload/
# 2. build the Setup app (build.rs packs port-undercover/ into payload/source.tar.gz and lists the mods' SHA-256)
cargo build --release
# 3. make "dist/Need for Speed Undercover Setup.app"
./make_setup_app.sh
```

`payload/pgo/game.profdata` is the speed profile the tested build was compiled with (how often each translated
function ran in play; no game code). `payload/rebased/PoliceCareer.asi` is the Police Career plugin as adapted to the
Steam `nfs.exe` (the mod author's code with its addresses rewritten), so the retail game program the mod ships with is
not needed. Headless test: `nfsu-setup --selftest /path/to/nfs.exe /path/to/output-folder --verbose`.

## License

The Setup app's own code is under the MIT license (see `LICENSE`). The port in `port-undercover/` keeps its own
`LICENSE` and `NOTICE` (MIT, vasist15-source and veritr1x). The mods belong to their authors. *Need for Speed:
Undercover* and its files belong to Electronic Arts; none of them are in this repository or its releases.
