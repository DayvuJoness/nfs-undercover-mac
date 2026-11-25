# Need for Speed: Undercover — Native for Apple Silicon Macs (work in progress)

A native macOS build of the 2008 PC game made by **static recompilation**: the game's own x86 code is
translated to C ahead of time and compiled for Apple Silicon, with a Metal renderer, native audio and native
controller support. No emulator, no Wine, no CrossOver. Built on the Need for Speed: Carbon Mac port
(vasist15-source, MIT) and veritr1x's recomp-kit.

> **You need your own copy of the game.** This repository contains no game code, executables, artwork,
> sound, movies or tracks. The build takes the files from **your** installation and makes the app on **your**
> Mac. Nothing it builds is meant to be shared.

This project is not affiliated with or endorsed by Electronic Arts. *Need for Speed* and *Undercover* are
trademarks of Electronic Arts Inc.

**Status: bring-up.** The exe is identified and the port's base is set up; the game is being translated and
brought up step by step (see CLAUDE.md, "Where we are").

## Which nfs.exe

The port is built for one exact `nfs.exe`: version 1.0.0.1 (December 2008, 10.5 MB), **without** SecuROM disc
protection (SHA-256 `d1ecded0…c7d2e9`). The 46.6 MB SecuROM-wrapped retail exe can't be translated; an unwrapped
1.1.2.1 exe was used first and dropped because it crashed on loading a save, on Windows too.

## Build it

```
./setup.sh "/path/to/Need for Speed - Undercover" --check-only   # only checks the folder
./setup.sh "/path/to/Need for Speed - Undercover" --out ~/Desktop
```

Steps: check the folder → link it (never written to) → relocate msvcr80/msvcp80 → analyse nfs.exe with
Ghidra (downloaded once, pinned hashes) → translate and build → package "Need for Speed Undercover.app".
Saves and settings go to `~/Library/Application Support/NFS Undercover/`.
