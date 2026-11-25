# Mechanics sheet: Undercover Setup (rebuilt from the Carbon Setup app)

## Where the values come from
The disk image holds `Need for Speed Carbon Setup.app`. Its program `Contents/MacOS/Setup` (arm64 Mach-O,
558 960 bytes, SHA-256 1700d1a6…9591, ad-hoc signed as io.github.nfsc-native-mac.setup) was carved out of
the APFS volume. All 35 code pages match the signature's hashes, so the copy is intact
(evidence ev_b0e11f98…). The program kept its Swift function names. Each function below was decompiled
with the built-in decompiler; Ghidra's decompiler isn't built for this Mac. The values were confirmed
against the program's data and instructions. The source carried inside the app
(`Resources/source.tar.gz` → `tools/installer/SetupApp.swift`, `tools/setup_kit/setup.py`) says the same
things. Addresses are virtual addresses in that program. None of its code was copied: the app's own code
is written from this sheet.

## Rules and values
| Rule | Value | Function (address) | How it was confirmed |
|---|---|---|---|
| Step line | trimmed line, length > 4, first char a base-10 digit, then `"/7 "`; name = line from the 5th char | `parseStep` 0x100001acc | `count < 5 → nil`, `prefix(1)` parsed radix 10, `starts(with: "/7 ")`, `dropFirst(4)` |
| Step count | 7 | `parseStep` 0x100001acc | literal `"/7 "` |
| Step weights | [0, 0.01, 0.01, 0.01, 0.50, 0.42, 0.01, 0.04] | `stepWeights` init 0x10000202c → static array 0x100034cc8 | 8 doubles read at 0x100034ce8 (0x3F847AE147AE147B = 0.01, 0x3FE0… = 0.5, 0x3FDAE147AE147AE1 = 0.42, 0x3FA47AE147AE147B = 0.04) |
| Overall progress | `step < 1 → 0`; else `sum(w[0..<min(step,7)]) + w[step] × clamp(within, 0, 1)`, then `min(…, 1)`; w[step] = 0 when step > 7 | `overallProgress` 0x1000020a4 | unrolled sum up to 7 terms, `fcsel` clamp, `fminnm` with 1.0 |
| Poll | every **2.0 s** while building | `build` 0x10000bed8 (`fmov d0, #2.0` at 0x10000cc50 → `scheduledTimer`) → timer block 0x10001e534 → `poll` 0x10000e0e8 | instruction |
| Poll rule | read last **65 536** bytes of `build/setup.log`; detail = parsed detail if any; within = fraction **only when step == 5**, else 0 | `poll` 0x10000e0e8, default `readLogTail(bytes:)` 0x100004d5c returns 0x10000 | `ccmp x8, #5`; `w0 = 0x10000` |
| Log tail | seek to end; start at `size − bytes` if size > bytes, else 0 | `readLogTail` 0x100004d64 | decompiled |
| Log progress | lines split on `\n` (empties dropped), newest first; a line starting `[` with `a/b` before `]`, both numbers and b > 0 → fraction a/b, detail "Compiling: Int(a) of Int(b)"; else a line with "Exported " and "functions" → detail "Analysing: " + text from "Exported " with `;`→`,` | `logProgress` 0x100004480 | `split(10)`, `starts(with:"[")`, `"/"` split count 2, `fcmp b, #0`, "Compiling: ", " of ", "Exported ", "functions", "Analysing: ", `;`→`,` |
| Output lines | text is joined to the leftover partial line, split on `\n`, last piece kept as partial; empty lines skipped; each step line resets detail and sets progress = overall(step, 0) | `consume` 0x10000d3e0 | decompiled |
| Kept log lines | **3000** (drop the oldest beyond) | `consume` 0x10000d3e0 | `cmp 0xbb9`, `count − 0xbb8` |
| Failure summary | from the last line containing "SETUP STOPPED" or "can't be used": that line and up to 3 more, joined by `\n`, "SETUP STOPPED: " removed; if it mentions "setup.log", add "\nClick Show the log to see them."; with no such line, the last **6** lines | `failureSummary` 0x100004148 | `prefix(4)`, `count − 6`, replace, string 0x100024b70 |
| Finish | stop timer, end activity; cancelled → phase cancelled + "Stopped. Nothing in your game folder was changed; Build again to continue."; exit 0 **and** `<dest>/<App>.app` exists → done, progress 1.0, "Built <App> in <elapsed>."; otherwise failed + failure summary | `finished` 0x10000dbc8 | phase codes 3/4/5, `0x3ff0…` = 1.0, `fileExists(dest + "Need for Speed Carbon.app")` |
| Phases | idle 0, checking 1, building 2, done 3, failed 4, cancelled 5 | `finished` 0x10000dbc8 | stored values 3, 4, 5 |
| Elapsed text | s = Int(seconds); s < 3600 → `"%d min %02d s"` (s/60, s%60); else `"%d h %02d min"` (s/3600, (s%3600)/60) | `elapsedText` 0x10000e438 | `cmp 0xe10`, format strings in immediates |
| Stop | only while the setup runs: mark cancelled, SIGTERM (15) every descendant (found with `pgrep -P`, deepest first), then terminate the setup | `cancel` 0x10000e390, `descendants` 0x1000052fc | `kill(pid, 15)` loop from the end |
| Stay awake | `beginActivity(options: 0x00FFFFFF = .userInitiated, which blocks idle system sleep, reason: "Building <App>")` | `build` 0x10000bed8 (0x10000c4f4) | instruction `mov w2, #0xffffff` |
| Environment | inherit, minus keys starting RECOMP_, NFSC_, POPM_, QA_, PYTHON and the key VIRTUAL_ENV; PATH = `/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin` | `setupEnvironment` 0x1000021e0, filter 0x10001d898 | small-string immediates "RECOMP_", "NFSC_", "POPM_", "QA_", "PYTHON", "VIRTUAL_ENV" |
| Setup command | `/bin/sh` with `[<work>/setup.sh, <game>]` + `["--check-only"]`, or `["--out", <dest>]` + `"--button-icons"` if icons + `"--hd-ui"` if HD UI | `setupArguments` 0x100003e18 | immediates "--out", "--button-icons", "--hd-ui" |
| Check | prepare source, run setup with --check-only; ok when exit 0 and a line contains "Check passed"; text "Your game is ready to build." or the failure summary; status shown while checking: "Checking… (the first time this also prepares the setup's own tools, about a minute)" | `check` 0x10000aa8c, worker 0x10000b36c | calls `prepareSource`, `setupEnvironment`, `contains`, `failureSummary`; string 0x100024c10 |
| Command Line Tools | present when `/usr/bin/xcode-select -p` exits 0; Install runs `xcode-select --install`, then polls every **5.0 s** until present | `hasCommandLineTools` 0x1000027b4, `installTools` 0x10000b210, timer 0x10001e3b8 | `fmov d0, #5.0`; runQuick status == 0 |
| Startup | tools checked at once (`runQuick xcode-select`, status == 0); destination /Applications when writable, else ~/Applications | `Setup.init` 0x100009a38 | runQuick at 0x100009fbc; fileURLWithPath + writable check |
| Quit while building | alert "Stop building the game?" / "The build stops now. Building again later continues from the one-time analysis if it finished." | `applicationShouldTerminate` (strings used at 0x10001efcc, 0x10001f008) | string references |
| Work folder | ~/Library/<X>-Native-Setup/source; an update keeps analysis, .venv, build, original, rebased, dist | `keptOnUpdate` 0x100001788, `prepareSource` 0x100002a28 | static set init |

## Undercover: what this installer carries and does (2026-11-25, from the person's working port)
Like the Carbon original, the installer carries the port's source and builds the game on the user's Mac from
their own nfs.exe, so no game code is distributed. The values come from the port's own files (its game.toml,
tools/setup_kit/setup.py and package.py, in port-undercover/), not estimates.

| Rule | Value | Source |
|---|---|---|
| What the user gives | only nfs.exe from the game folder copied from the Windows PC (or the folder, dropped) | person's request |
| Supported nfs.exe | 1.0.0.1, the Steam build, 10,584,064 bytes, no SecuROM, SHA-256 d1ecded0…c7d2e9 | port game.toml [game] sha256 |
| Runtime DLLs checked | msvcr80.dll 02bdde38…, msvcp80.dll 2026f3c4… | port tools/setup_kit/setup.py DLLS |
| Required folders / files | from the port's game.toml [setup] required_dirs and required_files | port game.toml |
| Payload (inside the app) | source.tar.gz (port source without original/, rebased/, analysis/, build/, dist/, .venv), mods_to_add/ (24 mods, 590 files) + mods_to_add.sha256, pgo/game.profdata | build.rs, make_setup_app.sh |
| Mods' place in the build | work/dist/mods_to_add (+ .sha256), where package.py and police_career.py read them | port package.py --mods default |
| Speed profile | work/build/pgo/game.profdata (the tested build's, 2026-11-17); the port's cmake uses it when present | port kit/cmake/Translate.cmake |
| Step 4 order | analyse nfs.exe + DLLs (analyze.py --only) → police_career.py adapts PoliceCareer.asi → analyse it | installer's port copy (analyze.py skips modules left out by --only) |
| Police Career check | adapted plugin's SHA-256 compared with the tested 5b275953…4e99; a difference is reported, not fatal | setup.py PC_TESTED_SHA256 |
| Step 6 | "Checking the built-in mods": every file's SHA-256 against mods_to_add.sha256; stop if any is missing or changed | setup.py check_mods |
| Step 7 | package.py --game GAME --out DEST --mods work/dist/mods_to_add | setup.py |
| Result | DEST/Need for Speed Undercover.app; saves in ~/Library/Application Support/NFS Undercover/ | port setup.py |
| Options removed | port source field; controller icons and HD UI (the port skips them for Undercover) | person's request; port setup.py |
