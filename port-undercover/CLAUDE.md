# NFS Undercover → native Apple Silicon (static recompilation)

Port of Need for Speed: Undercover (PC) to macOS with veritr1x's recomp-kit, built on the NFS Carbon Mac port
(vasist15-source, MIT; its kit/, tools/ and setup kit). The Carbon port's full notes (how NFSC.exe was brought
up, kit changes 1-28, every Carbon address) are kept in `docs/carbon-reference/CLAUDE-carbon.md`; its game-only
code (mods/core/nfsc, the HD UI tools) is in `docs/carbon-reference/` and is NOT built. Read those notes before
a kit change: most of Carbon's kit fixes (CRT through the IAT, D3DX volume/cube textures, effect-pool refs,
registry, focus queue, presenter) apply to Undercover as well, since the engine is the same family.
Explain findings plainly and keep commands copy-pasteable.

## Working rules (from the Carbon port, kept)
- Get evidence before patching: read the listing (`analysis/decompiled/<image>/functions/<addr>.asm`),
  the bytes (`xxd`), or the log line first. No guessing.
- Kit changes go in `kit/` directly, with a comment saying why.
- Never commit or copy game files or anything derived from them (original/, rebased/, analysis/, build/,
  dist/ are ignored). Nothing is ever written into the game folder.
- Rebuild: `.venv/bin/python tools/build.py` (add `--regenerate` only when the translator or game.toml changed).
- This checkout sits INSIDE the game folder (`original/` -> its parent). Tools that walk the game folder must
  skip this repository (package.py does); never run the build or game from inside `original/`.

## The exe (switched 2026-11-14)
- SUPPORTED NOW: `nfs.exe` FileVersion **1.0.0.1**, PE timestamp 2008-12-04 01:03:33, 10,584,064 bytes, SHA-256
  `d1ecded04cbba387c99299d2159b29b5c04b1d095293013caa4b404a43c7d2e9`; no SecuROM (sections .text 0x401000,
  .rdata 0xbd6000, .data 0xd1f000, .tls, .rsrc 0x1374000-0x1375220, .reloc; image end 0x1408000; entry 0x008aec55).
  Same imports as 1.1.2.1, same msvcr80/msvcp80 (identical SHA-256s). The person's install is at
  `<game folder>/new_nfsu/Need For Speed - Undercover` (`original` now links there; `original-1121-link` to the
  old one). Why: the person tried 1.1.2.1 on a Windows laptop and it crashed there too; this 1.0.0.1 install
  runs on Windows. Its data differs a little (CARS +2, LANGUAGES 112 vs 23, SOUND +2, MOVIES +4, EFFECTS +1);
  TRACKS is identical (MakeSceneryExtra.bin absent here too). Extra non-game files (DirectX/, pbsetup, an
  uninstaller, a release note, Steam-API stand-in files) are excluded from the bundle.
  Moving to it: every exe address in game.toml / core.nfsu was mapped by masked byte matching (one unique match
  each; /tmp tool mapfn.py, method in game.toml's header); the 1.1.2.1 configuration and mod are kept in
  docs/nfsu-1.1.2.1/. Kit changes all carry over (they are not exe-specific).
- FORMERLY: `nfs.exe` FileVersion/ProductVersion **1.1.2.1**, PE timestamp 2009-05-04 12:12:43, 15,918,352 bytes,
  SHA-256 `3f33193ff09a00cba0fb8b8a44ccac8fdc4133c12d1fa05251aff0164b6d355d`. No SecuROM: sections .text
  (0x401000-0xb6026b), .rdata (0xb61000), .data (0xcac000, vsize 0xbf2af4, raw 0x5e7000 -> bss tail), .tls,
  .rsrc (0x18a0000-0x18a1214, version + manifest only, no icon), .reloc; image base 0x400000, end 0x1939000,
  entry 0x0087ba75 (in .text). This is THE build the port is made for (confirmed by the user 2026-11-13: the
  unprotected 1.1.2.1 exe; which retail patch number it corresponds to is unknown). Pinned in game.toml [game]
  sha256 and checked by setup.sh / package.py.
- REJECTED: the retail nfs.exe 1.0.0.1 (2008-10-26, 46.6 MB, SHA-256 6caed72e…af95b4): SecuROM 7 wrapped
  (.securom section, entry 0x03070b08 inside it; paul.dll = SecuROM product activation). Its real code is not
  readable, so it cannot be translated.
- Imports: KERNEL32 (106), USER32 (48), ADVAPI32 (18; crypto + registry), d3d9 Direct3DCreate9, d3dx9_34 (12,
  incl. D3DXCreateCube/VolumeTextureFromFileInMemory, D3DXFillVolumeTexture, D3DXVec3Normalize), DINPUT8,
  XINPUT1_3 (#2 #4 #5), DSOUND #1, WINMM, WS2_32 (41), NETAPI32 Netbios, SETUPAPI, VERSION, SHLWAPI, SHFOLDER,
  OLEAUT32, ole32, RPCRT4, CRYPT32, imagehlp, WINTRUST, **MSVCR80 (171) / MSVCP80 (4)**.
- Runtime DLLs beside it: msvcr80.dll / msvcp80.dll 8.00.50727.762 (with microsoft.vc80.crt.manifest; msvcm80
  is the managed CRT, not imported). Preferred bases 0x78130000 / 0x7c420000 (inside the kit's extra heap) ->
  tools/rebase_pe.py relocates them to 0x10000000 (size 0x9b000, 6078 relocations) and 0x100a0000 (size
  0x87000, 6149 relocations). Stock SHA-256s pinned in tools/setup_kit/setup.py.
  The folder also has msvcr71/msvcp71 (not imported by nfs.exe), paul.dll/winui.dll (SecuROM, unused here),
  GDFBinary.dll / GameuxInstallHelper.dll (Windows Game Explorer), PB/ (PunkBuster).
- Registry key named in the exe: `Software\EA Games\Need for Speed Undercover` (values: Install Dir, g_Width,
  g_Height, g_Refresh, g_AudioMode, g_Brightness, FirstTime, g_ShaderDetailLevel, ... Language, SwapSize,
  CacheSize, CD Drive, StreamingInstall). Defaults/registry.json is empty until a run shows what is needed.
- Sound: EA RWAudioCore (same "RWAudioCore Dac" description strings as Carbon; the thread name not yet seen).
- The logo for the app icon: NFS_UNDERCOVER_LOGO (512x128 DXT3) in the player's FRONTEND/BOOTFLOWTEXTURES.BUN
  (game_art.py reads it at packaging time; no EA art is stored here). The player's GLOBAL/ holds only
  ATTRIBUTES.BIN and GlobalMemoryFile.bin.

## Layout
- `game.toml`: id nfsu, app UndercoverRecomp, bundle io.github.nfsu-native-mac, heap_base 0x01940000 (the image
  ends at 0x1939000, above the kit's default 0x01000000), 2 GB arena, `[modules.aux.a_msvcr80]` /
  `[modules.aux.b_msvcp80]`, `[setup] required_dirs`, `[bundle] exclude`, `[settings] core_mod = "core.nfsu"`.
- Sentinels and slots: the linker's "PADDINGXX" filler after the resources, 0x018a1400-0x018a1fff (hooks
  0x018a1f10.., animation counter 0x018a1f00, globals 0x018a1f60/64/70, zeroed by data_seeds). Mod slots from
  0x018a1400 up.
- `mods/core/nfsu/`: core.nfsu, bring-up stage: no hooks, only the settings the kit and launch.c read
  (pause_in_menu, upscaling, render_scale, mixer_own_core = off until tried).
- `tools/`: fetch_tools.py (Ghidra 12.1.3 + JDK 26 into ~/Library/NFSU-Native-Setup; reuses the Carbon
  port's pinned archives from ~/Library/NFSC-Native-Setup when their SHA-256 matches; builds the Apple Silicon
  decompiler once), analyze.py, rebase_pe.py, build.py/setup.py wrappers, setup_kit/ (setup.py = the 7 steps,
  package.py, launch.c, game_art.py, make_icon.py, defaults/).
- Packaging: `tools/setup_kit/package.py --game DIR [--out DIR] [--install]` -> "Need for Speed Undercover.app";
  launch.c profile `~/Library/Application Support/NFS Undercover` (registry.json, mod-settings.json, Logs/,
  ShaderWarm/), RECOMP_EXE = Resources/Game/nfs.exe.
- Setup protocol (the NFS-Carbon-Setup app's "Port source" field): `./setup.sh GAME --check-only` prints step
  lines and "Check passed. Nothing was changed."; `./setup.sh GAME --out DIR [--button-icons] [--hd-ui]` prints
  "1/7 …" … "7/7 …", logs to build/setup.log, "SETUP STOPPED: …" on failure, makes DIR/Need for Speed
  Undercover.app. --button-icons / --hd-ui are accepted and skipped (Carbon-only downloads).

## Kit changes for Undercover
1. host/present_thread.cpp: MetalFX upscaling setting read through `mods_core_setting("upscaling")` (the core
   mod game.toml names) instead of the hard-wired "core.nfsc/upscaling".
2. Translator, stale table sites: the late span-recovery pass recorded the table sites of a grown body before
   prepare() failed and never removed them, so fn_0046f090 / fn_0048ac80 were reported as "jump table ...
   decoded no entries at all" for code they never kept. A rejected growth now restores table_sites /
   table_sites_inferred / jumptables.
3. tools/analyze.py + tools/ghidra/CreateEntryFunctions.java: game.toml [translate] entry_points (and each
   module's) are made into Ghidra functions BEFORE its auto-analysis (-preScript), so their bodies and switch
   tables are listed. Without it 18 state-handler methods (0047fdf0 ... 004a7110, 0048ad20 ...) failed with
   "entry N -> ... is not an instruction boundary". After it: 845 table sites, 844 decoded, 0 gaps.
4. Translator: listed instructions outside executable sections are dropped (Ghidra's switch analysis of 00988cc0
   listed 670 "instructions" in .data at 0x01000100, with an ARPL); a listed function wholly in .data (0101159f)
   is not translated.
5. cmake/Dependencies.cmake: FFmpeg's configure refuses out-of-tree builds from a path with whitespace ("Need for
   Speed - Undercover"), so it is then built and installed in ~/Library/Caches/recomp-kit-ffmpeg/<md5 of the build
   dir>/ (ExternalProject PREFIX + --prefix there).
6. kernel32: InitializeCriticalSectionAndSpinCount (msvcr80's _mtinitlocks; DllMain failed without it),
   SetFilePointerEx / GetFileSizeEx (real), GetDiskFreeSpaceExA (4 GB free of 8 GB, like GetDiskFreeSpaceA),
   GetConsoleTitleA (0, 2 args: without a pop count the stack drifted and the game's /GS check ended it with
   0xC0000409).
7. d3dx9: the D3DX shims also registered as d3dx9_34.dll (Undercover's D3DX; MatrixTranspose, Vec3Transform,
   Vec3Normalize were "unimplemented"); D3DXGetPixel/VertexShaderProfile return "ps_3_0"/"vs_3_0";
   D3DXGetShaderConstantTable fails cleanly (E_FAIL, logged once) - NOT implemented yet.
8. user32 CallWindowProcA (calls the guest proc), RegisterDeviceNotificationA; misc.cpp stubs with honest
   "not available" answers and correct pop counts: ADVAPI32 Crypt hash/key, WS2 getaddrinfo (WSAHOST_NOT_FOUND) /
   freeaddrinfo, SETUPAPI (INVALID_HANDLE_VALUE / FALSE), SHLWAPI SHGetValueA (ERROR_FILE_NOT_FOUND),
   PathQuoteSpacesA, CRYPT32 / imagehlp (FALSE), WINTRUST WinVerifyTrust (TRUST_E_NOSIGNATURE), CoSetProxyBlanket.
9. SSE in the translator (the big one): nfs.exe is compiled for SSE with NO x87 fallback (77k MOVSS, 34k MOVAPS,
   6.4k MULPS, 5224 functions with packed SSE) - Carbon's kit trapped every packed form (14,415 traps in the first
   translation). Added: packed single ADDPS/SUBPS/MULPS/DIVPS/MAXPS/MINPS/SQRTPS/RSQRTPS/RCPPS/RSQRTSS/RCPSS,
   CMPccPS/SS, UNPCKL/HPS, MOVLHPS/MOVHLPS, MOVMSKPS, CVTPS2PD/CVTPD2PS/CVTPS2DQ/CVTTPS2DQ/CVTDQ2PS; packed double
   ADDPD/SUBPD/MULPD/DIVPD/MAXPD/MINPD/SQRTPD, CMPccPD/SD, MOVMSKPD, CVTDQ2PD, CVTSD2SI/CVTSS2SI (round to nearest);
   SSE2 integer forms on XMM (runtime/x86.h sse2_int: PADD/PSUB b/w/d/q incl. saturating, PMULLW/PMULHW/PMULHUW,
   PMADDWD, PMAX/PMIN, PAVG, PCMPEQ/GT, shifts by count incl. PSLLDQ/PSRLDQ, PUNPCK*, PACK*, PSHUFLW/HW), PEXTRW,
   PINSRW, PMOVMSKB. Tests: test_translate_insns.py cases "SSE packed single ..." and "SSE2 integer shifts ..."
   (Unicorn, 40 random runs each); 264 translator tests pass. Traps now: msvcr80 0, nfs.exe 107 (MMX forms on MMn
   registers - PADDSW/PMULHW/MOVNTQ/CVTPI2PS/CVTPS2PI ... - and MOVNTPS/MOVDQ2Q, likely the movie codec).
10. Translator: exports of an aux module in an executable section are entry points (pe_code_exports): msvcr80's
   getenv 1001f5ad sat inside Ghidra's FUN_1001f539 and was reached only through nfs.exe's IAT thunk 00ae4ec0.
11. Translator: the instruction-immediate scan also takes `MOV dword ptr [mem],imm` (code pointers stored to
   memory); 4 more entries. Unaligned starts right after a RET still fail its plausibility test -> listed by hand.

12. Raw shaders (dx/d3d9.cpp): CreateVertexShader / CreatePixelShader / Set* / Get* were D9_STUBs (D3D_OK without
   writing the out pointer). Now a K_D3D9SHADER object (IDirect3DVertexShader9 / IDirect3DPixelShader9: GetDevice,
   GetFunction) keeps the bytecode through END (comments by their length, SM2+ instructions by theirs) and its
   code_key; Set* put them into D9Pipeline vs/ps + vs_key/ps_key exactly as an effect pass does (NULL unbinds),
   clear vbinder/pbinder and the vs_prog/ps_prog cache. The kit's decoder + Metal translator draw them unchanged.
13. ID3DXConstantTable (dx/d3dx9.cpp, D3DXGetShaderConstantTable): reuses the effect code's ctab_parse (extended
   with type, elements, members, name/default offsets); handles = slots of a guest block (or a guest name string, as
   D3DX accepts); SetFloat/Vector/Matrix(+Array/PointerArray/Transpose)/Bool/Int/Value/Defaults write the pipeline's
   pconst/vconst/pbool/vbool (column_major matrices by column, row_major by row; Transpose swaps). INT4 not modelled
   (no ConstantI calls seen in Undercover's logs). Registered as d3dx9_26.dll and its aliases incl. d3dx9_34.dll.
   FOUND: Undercover's shaders (EFFECTS/SM3/SHADERS/ShadersLibrary.bin) open with a 'DBUG' comment and carry CTAB in
   the SECOND comment; ctab_parse only read the first, so every table failed (0x88760b59). ctab_find now walks the
   leading comments (effects benefit too). Test: d3d9_tests test_raw_shaders (synthetic ps_3_0 with DBUG + CTAB:
   create/bind/unbind/get/GetFunction, by-name and by-handle lookup, SetVector, SetDefaults, SetMatrix column-major,
   SetMatrixTranspose); suite 2288 checks, 0 failures (the test restores the pixel stage it changed).

## Addresses found (nfs.exe / msvcr80)
- Missed functions, all in game.toml entry_points with their evidence: 0046f1a0, 0046f300 and 18 state-handler
  methods (switch on `[esp+4]+3`, after int3 padding); 00a06100 (memory tracker setup, called from 00a03940; when
  missing, every category - Global, FE, Rendering, ... - was "not recognised" and a 160 MB pool failed with
  MemoryExhausted); 00880419..00880ac2 (CPU-dispatched copy family stored by 0088040d etc. into [0x1272c88..]);
  00a00400/00a2fe00/00a3d0c0; 0072b340 = the "gameframe" WndProc (CMP ESI,0xf on msg).
- msvcr80: 1000aa56 (after __create_locale), 100029e1 = _threadstartex (every _beginthreadex thread).
- 00a07820 = the game's debug printf (OutputDebugString); its callers are followed by INT3 (asserts).
- Memory tracker: object at 0x188e8e0 (built by 00a067a0), pointer [0x0189a1e8]; per-thread cache via static TLS.
- Threads seen: RenderThread (guest 2), "Job Manager - Job Thread" (3-5), 008890c0 (6), 0088af90 (7), 8, 9,
  0087c11f (timer thread, SetWaitableTimer without APC), 0088c3a0 (15, network lookup at scene load). The old
  "thread 7 calls address 0" was the kit's closed-handle start bug (fixed 2026-11-14, see Where we are).
- Scene load: FUN_005dc530 -> TRACKS\<track>\MakeSceneryExtra.bin; FUN_0068fe30 = file size or -1;
  FUN_00a07450 = pool allocate (debug fill 0xAA via vtable +0x2c -> 00a03360 -> memset thunk 0087afb6).

## Where we are
- 2026-11-13: exe identified; port base made (game.toml, globals.toml, tools, setup.sh, launcher, packager,
  core.nfsu stub). `./setup.sh GAME --check-only` passes. All three images translate (nfs.exe 49,7k functions,
  0 table gaps, 0 recovery errors) and build/UndercoverRecomp.app builds.
- Dev runs: `build/run.sh SECONDS LOG [level]` runs from build/run-game, an APFS clone of the game folder (so the
  game never writes into the real one; rebased msvcr80/msvcp80 copied in).
- Run 10 (latest): CRT init, the game's memory pools (582 MB in the extra heap), RenderThread + Job Manager threads,
  Direct3DCreate9, CreateDevice 1280x960 on Metal. Then: `IDirect3DDevice9::CreateVertexShader is not
  implemented`, `D3DXGetShaderConstantTable is not implemented`, `CreatePixelShader is not implemented`, and
  RenderThread SIGSEGV at EIP 0077c09f (a shader it never got). Carbon used D3DX effects only; Undercover makes
  raw shaders.
- Runs 13-15 (raw shaders + constant tables in): no crash any more; the game loads its shader libraries
  (EFFECTS/SM3/SHADERS/ShadersLibrary.bin, ShaderDeDupMapping.bin, EffectsLibrary.bin), starts "RWAudioCore Dac" and
  plays sound (0 late audio pulls), runs 4-7 minutes and exits cleanly (ExitProcess 0) when the window is closed -
  but presents 1 frame with 0 draws. 009eb5e0 (worker, called from 0080493a) was missing; the translator alone did
  not adopt it ("recovery did not validate"), so nfs.exe was re-analysed with the pre-create script (33 of 33
  functions made) - now 009eb5e0, 00a00400, 00a2fe00, 00a3d0c0 are all translated.
- Run 16/17 (latest): the main thread now gets further and faults in memset (thunk 0087afb6 -> msvcr80) writing at
  guest ~0x80000000, right after `CreateFileA(C:\Program Files\EA Games\Need for Speed Undercover/GLOBAL\
  GLOBAL_APT_FONT_HD.BUN): not found`. The player's GLOBAL holds only ATTRIBUTES.BIN and GlobalMemoryFile.bin; no
  *APT_FONT* file exists anywhere in the install. Also not found (probably harmless, as in Carbon): NFS\ZDIR.BIN,
  C:\bin.dat, HKLM ...\ergc.
- ROOT CAUSE (2026-11-13, static evidence; not the arena - it is already Carbon's 2 GB layout and the big
  allocations land fine): the player's current install is INCOMPLETE. Of 108 game-file names in nfs.exe's strings,
  29 are absent; in GLOBAL/: GLOBALA.BUN, GLOBALB.LZC/.BUN, InGameA.bun, INGAMEB.LZC/.BUN, PermanentMemoryFile.bin,
  InGameMemoryFile.bin, CarHeadersMemoryFile.bin, UNIQUEBOOTTEXTURES.BIN, DYNTEX.BIN, FE_ATTRIB.BIN, GAMEPLAY.BIN,
  NFSGD.BIN, HUDTEX*.BIN, NISOverlayTextures.BIN, cars_vault.BIN and GLOBAL_APT_FONT_HD/SD.BUN (also FRONTEND
  CHAL*.BUN and DEMO_* - perhaps edition-dependent). GLOBAL holds only ATTRIBUTES.BIN + GlobalMemoryFile.bin (dated
  2025-10-20, with this nfs.exe); the first install in this folder had ~29 entries in GLOBAL. GlobalMemoryFile.bin
  contains no APT font BUN (only texture names). The font is opened by 005dcfa0 -> 00692e20 with the name pointer
  at 0x00cbfacc (HD or SD by resolution); neither exists, and the main thread then faults in memset.
  setup.py now checks game.toml [setup] required_files (both fonts, the files a run proved necessary) and stops
  with "SETUP STOPPED: the game folder is incomplete ..." - `--check-only` on the current folder now fails.
- 2026-11-14: translator also handles MOVNTQ, MOVNTPS/MOVNTDQ (plain stores), CVTPI2PS, CVTPS2PI/CVTTPS2PI,
  MOVDQ2Q, MOVQ2DQ (test "MMX <-> SSE conversions and non-temporal stores"; 265 translator tests pass). Unmodelled
  traps now: nfs.exe 0, msvcr80 0, msvcp80 0. Run 18 (60 s wall clock): same stop - GLOBAL_APT_FONT_HD.BUN not
  found, then the memset fault. GLOBAL still has 2 files; no Undercover GLOBAL data anywhere on this Mac (only the
  Carbon apps' GLOBAL in ~/Downloads, which is Carbon data and must not be used).
- 2026-11-14 the person restored the full GLOBAL (29 files; the old 2-file one is GLOBAL_old in their folder,
  excluded from packaging by game.toml [bundle] "*_old"). `--check-only` passes again. build/run-game's GLOBAL
  refreshed (old one moved to build/run-game/GLOBAL_stale). No more font/memset crash; loading reached DYNTEX.BIN.
- BUSY-WAITS under the cooperative scheduler (found with RECOMP_PROFILE=1 + the new RECOMP_PROFILE_EVERY=N window
  report in kit/runtime/profile.cpp; `sample`/lldb could not attach): (1) main thread in FUN_0071cff0 spinning on
  FUN_007666f0 (flag [0x013b31e4] set -> returns at once) waiting for the render thread; (2) render thread
  (FUN_007664d0 loop at 007665e0) spinning on FUN_00687070 over the empty command ring at 0x1783ca0 waiting for the
  main thread. Windows threads end such spins by preemption; the kit hands the baton over only at a blocking import.
  core.nfsu (mods/core/nfsu/nfsu.c) now has BEFORE hooks on 007666f0 (when the flag is set) and 00687070 (when the
  ring has nothing to run) that call the game's own Sleep(0) through its IAT (0x00b610a0) -> both waits pass.
- Then: first real rendering (draws of 320x240 boot targets, Metal shader compiles, D3DXFillVolumeTexture via guest
  0073e690, "VideoDecodeThread" started). WS2_32 ordinals 8/9/11/12/14/15/101/151 added (ord9 = htons had no pop
  count -> stack drift -> fault at 0090ac35 in online set-up).
- Run 27/28 (latest): main thread faults in FUN_00926420 (called from FUN_0095a4e0 at 0095a536 with tag 'SMEA',
  0x41454d53) reading [audio System + 0xb0] = 0xac9016ca. The object is rw::audio::core::System from FUN_0092c3b0
  (pointer [0x0187a420]; +0xb0 = "mpObjectIdRegistry", allocated there). Audio runs, so init succeeded: the field
  was overwritten later (no kit heap-guard report; RECOMP_WATCH_FRAME shows only the msvcr80 SEH prolog/epilog).
  NEXT: find the writer of [[0x0187a420]+0xb0] (a watch on that guest address from a hook, or a write-check), then
  the menu. Also seen, not yet chased: thread 7 calls address 0 at start (network idle thread, as in Carbon).
- 2026-11-14, the 'SMEA' fault, measured (the person's hypotheses checked one by one):
  * no "call to unknown target" other than thread 7's address 0 in runs 26-28 -> no new entry points to add;
  * RECOMP_WATCH=0187a420 (kit software write watch): the audio System pointer is written ONCE (= 047c6640, by
    fn_0092c3b0), never again; RECOMP_WATCH=047c66f0 (System + 0xb0): written twice, 0xaaaaaaaa by fn_00a03360 (the
    game's allocator fill) then 0x04806800 by fn_0092c3b0 - never overwritten. So the System object is fine;
  * the fault is in the block after 0095a536 (kit EIP 0095a53b): `MOV EDI,[ESP+0x24]` (arg 1 of FUN_0095a4e0) ...
    `MOV [EDI+0x50],ECX`. The bad address follows the heap layout: 0xac9016ca normally, 0xac9046ca with
    RECOMP_HEAP_QUARANTINE=1 (+0x3000, as the heap shifts) = a heap base + 0xaaaaaaaa: a self-relative offset field
    of a freshly allocated block still holding the game's 0xAA fill is added to its base (FUN_0095a760 adds
    [blk+0x20], [blk+0x24], [blk+0x30] to the block address). I.e. the block's data was never filled in (a read not
    done yet, or done short), not a use-after-free.
  * caller chain: FUN_00497490 -> FUN_0095a760(block = [[[0x01294638]+0xf0]+0x1c]+8, or [[0x01294638]+0xf0]+0x20,
    0, 0, 00490720) -> FUN_0095a4e0(block, block+[block+0x30], ...).
  NEXT: identify the resource behind [[0x01294638]+0xf0] (which file, which loader) and why its contents are not
  there when 00497490 runs: an async read that has not completed under the cooperative scheduler (another
  busy-wait or a wait that returned early), or a failed/short ReadFile (check CreateFile/ReadFile results around it
  with LOG=2 in a 60 s run).
- 2026-11-14 SOLVED, the 'SMEA' fault: a core.nfsu diagnostic hook on FUN_00497490 (removed after) logged the
  bank blocks: 4 good ABKC banks, then descriptor 0x012950e0 (24th of a static 0x5c-stride table from 0x01294840)
  with name hash 0, size 0xc0, block all 0xAA. With LOG=2 the last open before the fault was the DIRECTORY
  "SOUND\IG_GLOBAL\" (the unnamed bank's empty file name). Windows refuses CreateFile on a directory without
  FILE_FLAG_BACKUP_SEMANTICS (ERROR_ACCESS_DENIED) and the game skips the bank; the kit opened it as a file, read 0
  bytes, and the game used the unfilled block. kit/runtime/kernel32.cpp create_file_named now refuses a directory
  without 0x02000000 (kit change 14). After it: no crash, the 24th bank loads normally (ABKC at 03ba0200).
- Cop speech (found on the way, kept): the game asks for Sound\Speech\copspeechdat_en.big whatever its registry
  Language (read by 007318d0 from HKLM\Software\EA Games\Need for Speed Undercover; table 0x00cd8b10, 12-byte rows
  {name, id, ...}, Russian = row 19, id 0xf). The person's Russian install has only copspeechdat_ru.big (en has
  hdr/sth but no dat). Kit change 15: RECOMP_FILE_FALLBACK="DIR|PREFIX|PROBE_%s|CODE|ALTS" switches a whole file
  set to the one language present (generic; the rule string is in launch.c and build/run.sh). package.py writes
  Language = the installed speech language into Defaults/registry.json (detected from copspeechdat_<code>.big).
  build/run.sh sets RECOMP_REGISTRY to the dev profile (a developer build reads build/registry.json otherwise).
- Kit change 16: RECOMP_PROFILE_EVERY=N prints the profiler's top functions every N s (runs killed by the timer
  never reach the atexit report).
- FIRST PICTURE (run 37/38, 2026-11-14): no crash, ~57 fps presented (3306-3376 frames in 59 s), clean exit.
  RECOMP_HOST_DUMP_EVERY=300 frames show the game's Russian safety-warning screen from 10 s to 55 s; the profile is
  a normal frame loop (FUN_009773b0 = frame wait through SleepEx ~58%), so it is waiting - most likely for a key.
  Packaged dist/Need for Speed Undercover.app runs the same (1521 frames in 40 s), Language = Russian by default;
  the game wrote its own settings into the profile's registry.json.
- BLACK SCREEN FOUND AND FIXED (2026-11-14, the person saw black + no sound in the packaged app): frames were fine
  (dumps showed the warning text), the screen was black because the game's gamma ramp was all zeros (new one-time
  log in Dev_SetGammaRamp: red 0/64/128/255 = 0/0/0/1). The ramp is built by FUN_00740ba0 ((WORD)(pow(i/255,
  0.81) * 65535 + 0.5), called from 0075efb0 with [0x00ce11d8]); RECOMP_TRACE_XMM_CALL=100570e3 (new temporary trace
  in kit runtime/cpu.cpp recomp_module_call) proved msvcr80's __libm_sse2_pow right. The generated C showed the
  TRANSLATOR BUG (pre-existing in the kit): CVTTSS2SI read the register as a double (`xmm_f64`) because the
  single-precision rule only looked for a trailing "SS". Fixed in translate.py + Unicorn test "CVTTSS2SI truncates a
  single ..." (266 translator tests pass). ~900 CVTTSS2SI sites in nfs.exe were all wrong before.
  After it: ramp 0/21389/37499/65535; the packaged app shows the warning, then "Загрузка..." with the logo, then the
  title screen "Нажмите клавишу ENTER" (frame dumps at 15 s and 25 s). No sound on the warning screen is normal: the
  game plays silence there (peak 0.000).
- 2026-11-14, the person's report: picture shows, no sound, crash after Enter on the title screen (2 runs, same):
  "call to unknown target 0088665a (return 00884cd9) ... returning 0" then unhandled AVs. 0088665a = a thiscall
  method right after a `RET 0xc` (no padding), reached through a vtable -> game.toml entry_points, re-analysed
  (34 of 34 pre-created), now translated. NOT yet verified by pressing Enter (macOS refuses scripted keystrokes for
  osascript here: no Accessibility permission) - the person tests.
- Sound, measured: RECOMP_AUDIO_DUMP (raw 48 kHz stereo of what the guest writes into its stream ring) shows real
  signal from ~12 s to ~21 s (loading/logo, peaks up to 7058) and silence from ~24 s on the title screen, although
  NFS09FE.mus (frontend music) is opened there. The one refused DirectSound buffer is the game's 5.1 probe (6 ch,
  0 bytes, 0 Hz; flags 00018000) - Windows refuses it too; the game then uses 48 kHz stereo. dsound now logs each
  refused CreateSoundBuffer with its parameters. Open: whether the title screen is silent in the original or music
  should play there (check after Enter works).
- NEXT: the person opens the app and presses Enter on the title screen; then the EA logo movie
  (UI/EA_Bumper.big and SUBTITLES/EA_Bumper.sub are not in this install - check what the game does without them),
  press-start, the menu. Thread 7's null call (network idle thread) is still there, harmless so far.
- NEXT (old): the person restores the full installed GLOBAL folder (their earlier copy had it); then re-run
  (`build/run.sh 60 LOG` now ends the game by the wall clock - RECOMP_MAX_SECONDS only counts presented frames).
  Then the 107 MMX traps (movies), thread 7's null call, registry defaults, the menu.
- 2026-11-14, the person's report: save loads ("загрузка завершена"), then the scene loading screen never ends;
  minimizing crashed; no sound.
  * Minimize crash: XINPUT1_3 is imported by ordinal; ord5 (XInputEnable, 1 arg) had no argc ->
    TerminateProcess(0xC0000409). kit/dx/xinput.cpp maps ord2..ord8 to the existing shims.
  * Sockets: socket/ord23 now return INVALID_SOCKET and WSAGetLastError/ord111 WSAENETDOWN (10050) instead of 0.
  * Host: RECOMP_KEY_SCRIPT="SEC:Key,..." (kit/host/sdl/main.cpp key_script_tick) presses keys at given seconds,
    so a run can go through the title/save screens unattended (osascript keystrokes are not permitted here).
    Test profile "~/Library/Application Support/NFS Undercover Test" = a clone of the person's (save "Player1");
    `NFSU_PROFILE="NFS Undercover Test" RECOMP_KEY_SCRIPT="32:Return,40:Return,48:Return" build/run.sh 60 LOG`.
  * ROOT CAUSE 1 of the endless loading (kit bug, kernel32.cpp): the guest does CreateThread, SetThreadPriority,
    CloseHandle at once (0088b25d..0088b26a); the worker read its start address from the HANDLE when it first got
    the baton, found it closed and ran guest_call(0). That was "thread 7 calls address 0" and also thread 15
    (0088c3a0, the network/lookup thread started when the scene loads) -> both died at once, and the loader waited
    for them for ever. Found with a new log in guest_call for targets < 0x10000 (host caller via
    __builtin_return_address + atos -> thread_host_main) and per-return-address null-call keys in cpu.cpp.
    Fix: GuestThread keeps start/param from CreateThread. Test in runtime_tests ("a thread whose handle was closed
    before it ran still ran its body"); runtime_tests 1372 checks, 4 failures - all pre-existing (pefile helper,
    msvcp80 GetProcAddress by mangled name, unknown-argc coverage), none from this change.
  * STOP 2 (open): after the fix the threads run (gethostbyname/ord52 fails honestly) and the scene load asks for
    TRACKS\L8R_MW2\MakeSceneryExtra.bin (FUN_005dc530 = slot 1 of the vtable at 0x00b86614, string 0x00b86654,
    loaded via 00692e20 -> 006906c0 -> 00690620). The file is NOT in the retail install - CORRECTED: the person
    restored the original TRACKS (identical, diff -rq) - so the game must find it elsewhere on Windows; it is not
    a required file (taken back out of required_files). Chain measured with core.nfsu's opt-in diagnostics
    (NFSU_DIAG_MEMSET=1): CreateFileW fails (correct, INVALID_HANDLE_VALUE) -> FUN_0068fd80 returns 0 ->
    FUN_0068fe30 returns -1 -> 00690620 stores +0x48 = -1 (unsigned CMP 8/JNC) -> 00692da0 -> 0068f490 ->
    004a9fb0 -> 00690cb0(size -1) -> pool[1] at 0x10130000 (vtable 00b9a464, alloc 006889a0 -> 00a032c0 ->
    00a02cb0/00a02560, dlmalloc-like, no max-request check: a 16-byte block) -> FUN_00a07450 fill (flags 7, bit 2
    = clearalloc=0xAA from the embedded MemoryConfiguration text used by 0068f9c0 -> 00a06780/00a06100, same in
    retail) -> memset(-1) -> SIGSEGV at 0087afb6. Every step is what Windows would do, so on Windows
    FUN_0068fd80 must succeed: FUN_0068fbd0 first looks the name hash up in the packed-file tables (list sentinel
    0x00cc98c8/0x00cc98cc, add = FUN_00689c30, find = FUN_00689c80, rows of 5 dwords: hash, ..., size at +8).
    MEASURED: a table (guest 1f578f80) is added with 19 rows from 0068a1ba at boot and with 144 rows from 0081850b
    at scene load, but when the size query runs the list is EMPTY (0 tables; name hash via FUN_00683c00 =
    43b6802c). NEXT: who unlinks the table before 005dc530 runs (hook the unlink / watch 0x00cc98c8 with
    RECOMP_WATCH), and whether on Windows the scenery bundle's table is linked at that moment (ordering under the
    cooperative scheduler, as with the earlier busy-waits).
    Also absent in the install but not proved needed: FRONTEND\CHAL*.BUN (5), GLOBAL\HUDTEXSPLIT.BIN,
    TRACKS\PrecullerBooBooScript.hoo.
- 2026-11-14 (later), on 1.1.2.1 before the switch: the core.nfsu MakeSceneryExtra hook (size -1 -> 0 for that
  file only) got the scene load past the memset; then 19 missing vtable methods (added) and msvcr80's x87
  dispatcher targets (__trandisp2 `JMP [EBX]` tables 0x100929c4/0x100931b0: 1005be34 ... 1005c9f8, added to the
  msvcr80 entry_points; it aborted with "no block entry for indirect jump to 0x1005c9d5"); then the audio thread
  ("RWAudioCore Dac") wrote through a garbage BE-PCM packet header in FUN_009627d0. The person then reported that
  1.1.2.1 crashes on Windows as well and supplied the 1.0.0.1 install - see "The exe".
- 2026-11-14, 1.0.0.1 bring-up:
  * TOOL BUG FIXED: tools/analyze.py left the previous analysis's per-function listings in
    analysis/decompiled/<image>/functions (Ghidra's exporter never deletes), so 1.1.2.1 bodies were translated
    into the 1.0.0.1 build (00597920.asm read the old cookie address 0x01272378). Each image's listing folder is
    now emptied first. (The first 1.0.0.1 failures - 91 "dispatches to ... not an entry point", 10 switch
    tables - came from that mix; the ten switch functions 004341f0 ... 007d99a0 stay in entry_points, harmless.)
  * Missing functions added (unknown targets, then 0xC0000409): 0049a9f0, 0082f610, 005f75b0.
  * Runs: core.nfsu hooks install at 00792180 / 006a2cd0 / 006abab0; 1 frame presented, then
    TerminateProcess(0xC0000409) from the CRT's __report_gsfailure. NFSU_DIAG=1 (core.nfsu: hook on
    __security_check_cookie 008ae230, cookie 0x00d61c88) names the failing function: FUN_00597900 (frontend;
    returns to 00597ac1), slot [entry ESP - 0x10] (main stack 0x0efffe58 in these runs) holds ~0x00bee110 (an
    .rdata-range value) instead of cookie^ESP. With RECOMP_WATCH=0efffe58 the run did NOT fail in 60 s (slower;
    the crash looks timing-dependent - another thread writing into a dead stack object?). Writers of that slot
    seen: fn_00538d40 (1867), fn_008c84a0 (211, writes 0x00be3108), fn_008ae30e, fn_008c7f00.
  * NEXT: find who writes 0x00bee11x into the main stack while FUN_00597900 runs (watch with a narrower window,
    e.g. a core.nfsu hook that records [slot] after each call in 00597900, or RECOMP_WATCH + a value filter);
    check threads other than 1. Then the save/scene load on 1.0.0.1, sound, packaging check by the person.
  * The old app (1.1.2.1) is kept in dist/old/; dist/Need for Speed Undercover.app is the 1.0.0.1 build.
  * SOLVED, the 0xC0000409 after the first frame: not a stray write but STACK DRIFT. ECX at the failed check was
    the same in every run (cookie random), and the slot's content XOR ESP = 0x00bee110, a string FUN_005717b0
    pushes. An ESP trace (core.nfsu NFSU_DIAG hooks) showed FUN_0069da40 returning 4 bytes low: its callee
    FUN_0069d920 does `PUSH 0x10; CALL [0x00bd61f4]` = KERNEL32!GetUserGeoID, which the kit did not shim ("has
    an unknown stdcall argument count: not adjusting ESP" in the run log; 1.1.2.1 never imported it). Kit change:
    k_GetUserGeoID (1 argument; 244 = United States for GEOCLASS_NATION, matching the kit's en-US LCID) +
    runtime test "GetUserGeoID(GEOCLASS_NATION) is the United States and pops its argument" (1391 checks, 5
    failures, all pre-existing: pefile helper, msvcp80 mangled GetProcAddress, IAT data symbols, Delphi argc,
    4 other unknown-argc imports). No other import in the run logs has an unknown argc.
  * NEXT STOP (open): main thread SIGSEGV in memset (thunk 008ae1a6 = MSVCR80!memset) filling -1 bytes with 0xAA,
    same mechanism as on 1.1.2.1: FUN_004aff10 (sound start-up; block runs because [0x00d3cec0] = 1 statically)
    -> FUN_004afb80 loads 4 sound-event banks: FUN_0049ff20(key 0..3, folder 3 = "sound\evt_sys\") takes the
    bank name from the attribute collection handle [obj+0x9c] (obj = [0x00d83cd0], handle from FUN_0049edf0 =
    collection class 0xd3c18f03, name 0x6367e03d, field 0x4166fa80[key]). The collection is NULL because the
    attribute database [0x00d834d0] is NEVER created (RECOMP_WATCH: no write; the only writer is 0046844f in
    FUN_004683e0 = slot 2 of Attrib::DatabaseExportPolicy, vtable 0x00bdf060). The "db" vault IS loaded at
    boot (006ac470 entered, 00468df0 policy getter and 00466c70 run; both hashes are in GlobalMemoryFile.bin),
    but the export's policy slot 2 is never called. So the name is "", the folder itself is "opened" (refused
    like Windows), size -1, the pool allocates/fills -1 bytes. NEXT: find what calls a policy's slot 2 (vault
    export initialisation, probably in 00466ec0's fix-up switch or a later Vault::Initialize step) and why it
    does not run - a missed function/translation gap or an ordering issue (the vault stages run later).
    Diagnostics kept in core.nfsu under NFSU_DIAG=1: cookie check, ESP trace, memset, bank lookup, vault and
    attrib entry logging.
  * SOLVED 2026-11-15, the missing attribute database: TRANSLATOR BUG (kit). The vault fix-up 00466ec0 binary-
    searches a (type hash -> export policy) table that 00468df0 builds from three globals set by C++ static
    initializers: [0x00d83508] Attrib::DatabaseLoadData (00bbcd00), [0x00d83504] (00bbcd20), [0x00d8350c]
    (00bbcd40); the database's was 0, so its policy (slot 2 = 004683e0, the only writer of [0x00d834d0]) never
    matched export type 0xcbbc628f. 00bbcd00 never ran: the initializer before it, 00bbccd0 (recovered by the
    translator, not in Ghidra's listing), contains `MOVSD XMM1,qword ptr [0xbd9750]`, and translate.py's
    to_insn dropped the operands of every string mnemonic, so it was emitted as the STRING MOVSD - XMM1 never
    loaded, ESI/EDI += 4, and ESI is msvcr80 _initterm's cursor, so the next table entry was skipped. Fix:
    to_insn keeps operands when one names an XMM register (same rule as names_an_xmm for listings). Test:
    test_translate.py test_recovered_sse_movsd (F2 0F 10/11, F3 0F 10 keep operands; A5, F3 A5 keep none).
    After it: the database exists, the four evt_sys banks get their names (MAIN_AEMS.csi ...).
  * Missing functions, found in bulk (2026-11-15) instead of one crash at a time (tools in /tmp, method):
    every RTTI vtable (3192; dword before = CompleteObjectLocator of a .?AV/.?AU class) slot that starts after
    int3/RET/RET n and is absent from build/recomp/gen/table.c (61 found), the data-section pointer scan
    (5 more) and the code-immediate scan (PUSH/MOV of such an address; 7 more), plus the tail-jump targets of
    007e2360. All in game.toml entry_points with their evidence. Exception-handler bodies (`MOV ESP,[EBP-0x18]`)
    and data-looking hits are left out on purpose.
  * KIT FEATURE: D3D9 hardware instancing. SetStreamSourceFreq/GetStreamSourceFreq were stubs; NFS Undercover
    draws repeated scenery/traffic instanced, which came out as huge stray polygons. Now D9Pipeline::Stream.freq
    -> HostD9Stream.freq; Metal: INSTANCEDATA streams use MTLVertexStepFunctionPerInstance (stepRate =
    freq & 0x3fffffff), the vertex-layout key includes it, and indexed draws use instanceCount from stream 0's
    INDEXEDDATA count. Test d3d9_tests test_stream_frequency (2307 checks, 0 failures).
  * Host: RECOMP_KEY_SCRIPT takes an optional hold time, "SEC:KEY:HOLDSEC" (e.g. 160:Up:40 drives).
  * RESULT (240 s scripted runs, test profile "NFS Undercover Test", Enter at 32..140 s, Up held 160..200 s):
    warning, logo, title "Нажмите клавишу ENTER", save loaded ("Загрузка завершена"), the intro cutscene, then
    FREE ROAM DRIVING with HUD (160 km/h, minimap, heat, "Проезд впритирку") and correct scenery; no crash, clean
    exit when closed. Audio (RECOMP_AUDIO_DUMP): real signal on the title screen, menus and in play (peaks to
    ~21000), silence only while loading. Translator insns tests 78 passed; runtime_tests 1391 checks with the
    5 known pre-existing failures.
- 2026-11-15 (afternoon), after the person's first real play (lag, no Silvia, one resolution, doubled speech, camera):
  * PERFORMANCE: the scheduler report showed thread 10 ("RWAudioCore Dac", the mixer) holding the baton 11-18 s
    of every 30 s and thread 8 (008be160, the network thread) 10-17 s: WSACreateEvent was a stub returning 0 and
    WSAWaitForMultipleEvents "succeeded" at once, so the network loop never blocked. Kit change: the five WSA event
    imports are kernel events (kernel32.cpp ws_WSACreateEvent / ws_WSAWaitForMultipleEvents; runtime test).
    Mixer on its own core is now the default (core.nfsu/mixer_free, renamed from mixer_own_core so profiles that
    saved the old default 0 get it). Result: 45-60 fps in play (was 6-25), 28713 frames in 480 s.
  * MISSING SILVIA: not a port bug. Both of the person's saves were written by the 1.1.2.1 exe; with them the
    player-vehicle list ([0x00de93d4]) stays empty after the office video (no CARS\NIS_240_SX_89 load) and every
    frame calls methods on a null car. A save made by 1.0.0.1 loads NIS_240_SX_89 and the test drive works. The
    person must start a new career.
  * RESOLUTIONS: d3d9 GetAdapterModeCount/EnumAdapterModes now offer 17 modes (4:3, 16:9, 16:10) at 60 Hz in
    X8R8G8B8; GetAdapterDisplayMode stays 1280x960.
  * UNITS: GetUserGeoID answers RECOMP_GEOID; the launcher sets it from the Mac's region (CoreFoundation,
    launch.c mac_region_geoid; package.py links CoreFoundation). Russia -> 203 -> km/h in a new profile.
  * TRANSLATOR (recovered code, Image.to_insn): LOCK no longer drops operands (157 LOCK XADD etc.); capstone
    FCOMPI/FUCOMPI aliased to FCOMIP/FUCOMIP. Found by /tmp decoder_diff.py: the capstone decoder vs Ghidra's
    listing over all 28.6k listed functions (remaining differences are cosmetic). Scalar CVTTSD2SI/CVTTSS2SI
    use sse_cvtt_i32 (NaN/out of range -> 0x80000000 as on x86; Unicorn case added). 79 insns tests pass.
  * Controls: [controls.native] xinput = true, dinput = false (the DirectInput layout put the triggers on Rx/Ry).
  * GetCursorPos answers the window centre until something places the cursor (was (0,0)).
  * CAMERA - FIXED (evening): the chase camera (DrivingCameraMover, rope model in FUN_004d57d0) takes its pitch
    from _CIsin. The translated FSIN left C2 as it was; the game enters _CIsin with C2 set from an earlier compare,
    msvcr80's x87 sin (10054748) tests C2 after FSIN, "reduces" and takes FSIN again: sin(sin(x)), so the camera
    pitched itself onto the roof / into the sky and the physics saw wrong angles too. Fixes: FSIN/FCOS/FPTAN/
    FSINCOS clear C2 for |x| < 2^63 (x87_trig_in_range; insns test with C2 left by an unordered FUCOM); x87_set_cw
    keeps bit 6 set and bits 7/13-15 clear as the hardware reads it back (0x023F -> 0x027F), so _CIsin/_CIcos take
    their SSE2 path as on Windows; MINSD/MAXSD/MINSS/MAXSS kept the wrong operand (MIN gave the larger; test).
    Tools: tools/dev/insn_fuzz.py (every SSE/x87 form in a module's listings vs Unicorn, random registers/memory/
    condition bits; --all/--range for a function), tools/dev/func_diff.py (one or more whole functions of a PE vs
    Unicorn with the image mapped, x87 or XMM0 inputs, --driver FLD/CALL/FSTP). Known Unicorn quirks, not ours:
    FPREM1 (no quotient rounding), FCOM keeps C1, invalid FSQRT/FYL2X status, RSQRTPS NaN sign.
    Diagnostics in core.nfsu: NFSU_CAM_LOG (rope direction per call), NFSU_TRIG_CHECK (now cos/sin/acos/sqrt/atan2
    against the host), NFSU_SIN_SNAP (arena snapshot at the first in-snap sin), NFSU_PROBE.
  * SPEED (late evening), measured with the new load journal (RECOMP_PROFILE=1 RECOMP_PROFILE_EVERY=5
    RECOMP_PROFILE_LOG=<file>: fps, baton share per guest thread, top guest functions and host imports per thread):
    (1) guest memory accessors are typed one-member structs (unsigned _BitInt in the generated C) so a guest store
    no longer makes clang reload every X86 register; game.toml [translate] store_hooks = false drops the watch/
    dirty hooks from the translation (RECOMP_WATCH no longer sees translated stores). (2) d3d9 VB/IB Lock/Unlock
    keep the guest copy from the 2nd lock on and write back / re-upload only the locked range (were a whole-buffer
    alloc+copy+upload per lock: 1/3 of the render thread, 8 fps on breaking props). (3) scheduler: each
    GuestThread waits on its own condition; sched_notify_locked wakes only the holder + g_sched_cv waiters
    (LeaveCriticalSection/hand-offs woke a dozen threads each time; CS shims were ~10% of the main thread).
    Result on the scripted drive: 30-40 fps with dips to 8 -> 55-60 with props breaking. Game time = real time
    (distance/speed ratio 0.98-1.00). frame_limit 120: sim and camera stay at 60 Hz (004e0e30 60/s), only the
    frontend loop (006ae4c0) runs 120/s, so 120 gives no smoother play and doubles frame-counted menu motion.
  * 120 FPS: d3d9 Query_GetData answers an occlusion count still on the GPU with that query's last count (the
    render thread polled until after Present; RECOMP_D3D9_QUERY_WAIT=1 restores S_FALSE). core.nfsu menu_60
    (default on): while the chase camera (004e0e30) has not ticked for 250 ms, each main-loop pass (006ae4c0)
    waits for its 1/60 s slot through the game's Sleep, so frame-counted menus keep their speed at frame_limit 120.
    Scripted drive at 120: 64-120 fps (main ~60% + render ~40% of one core, still serialised by the baton).
- 2026-11-16 night (the person: render on its own core, Mac resolutions, 120 fps, a working app):
  * RENDER THREAD ON ITS OWN CORE (core.nfsu render_free, default on; launch.c adds "RenderThread" to
    RECOMP_FREE_THREADS). Kit: d3d9/d3dx9 imports are free_ok and run under g_d3d_m (pure D3DX math unlocked);
    heap_* under a recursive mutex; g_last_error per thread; TlsGet/SetValue, Get/SetLastError, Heap*,
    SetEvent/ResetEvent/ReleaseMutex/WaitForSingle/MultipleObjects/SleepEx/IsBadRead/WritePtr free_ok (free
    waits wait on g_sched_cv); free threads ask for QOS user-interactive. Translator: [translate] memory_fences
    (full barrier before listed instructions) at the command rings' push/run/reset (006a2cc5, 006a2cf7,
    006a2d07, 006a2c50) and the reply flag [0x00ea0164] reads. core.nfsu throttles the render thread's idle loop
    (00791fc0): message pump FUN_0074be20 at most every 10 ms (it took the baton ~100000/s), idle SleepEx(0)
    sleeps 50 us. 6-minute runs clean. Imports that send a free thread back to the baton are counted in the log.
  * MAC RESOLUTIONS: the game's own tables decide - menu table 0x00be9658 ({w,h,label hash} x14, "User/Resolution"
    index) and the accepted-modes table 0x00c1f2a0 ({w,h} x15, FUN_00752fc0). core.nfsu mac_resolutions writes
    the screen in pixels/points (via the host's host_display_screen_size, dlsym) and the notch-free full-screen
    size into the 640x480, 800x480, 1152x720 and 1280x1024 slots of both; d3d9 adds the same sizes to the adapter
    modes. 3024x1964 runs with MSAA x2 at ~4 ms GPU. The person's registry set to 3024x1964 (backup
    registry.before-3024.json); a working 1.0.0.1 save "ru" copied beside their old (1.1.2.1, carless) saves.
  * MENUS AT 60 (core.nfsu menu_60): while the chase camera 004e0e30 is idle, main-loop passes 006ae4c0 wait for
    1/60 s slots. Occlusion queries answer the last count instead of S_FALSE (render thread polled ~5%).
  * MEASURING AT NIGHT: with the person away the lock screen / aerial wallpaper keeps WindowServer at ~94% CPU and
    the presenter shows ~70 fps whatever the game does. Count game frames as shown + dropped (d3d9 "frame
    pacing" lines; /tmp/gamefps.sh) and use a profile restored before each run (saves autosave and move the
    start). Bench (Fresh2 save at 3024x1964, 120 limit): ~104 game fps in the heavy start area, main thread bound.
  * Presenter shows queued frames oldest first (RECOMP_PRESENT_LATEST=1 restores newest-only); RECOMP_PRESENT_TIMING.
  * native/nfsu_native.c replaces FUN_0074a1d0 (box vs 6 planes) for the common case; NFSU_NATIVE_CHECK=1:
    15.8M calls, 0 differences; built with -ffp-contract=off. Small gain (the journal over-weights small calls).
  * Load journal now also samples free threads ("thread N (own core)"); macOS `sample` gets only 2-3 samples here.
  * OPEN - doubled speech: likely ring underruns while the mixer took turns; re-check with the free mixer.
- 2026-11-16 (the person: "menus still run like a 2x video", "it started after one of your fixes"; step 1 of 3):
  * ROOT CAUSE - TRANSLATOR ALIASING BUG (mine, the 11-15 evening speed-up): guest accessors RecompG16/32/64/
    GF32/GF64 were distinct C types (unsigned _BitInt(N), float, double), so clang treated a 32-bit store and a
    64-bit load of the same guest address as unrelated and read stale memory. NFS Undercover's frontend (EA APT)
    times each update in FUN_00a1c960: RDTSC delta stored as two dwords, FILD qword, / ticks-per-ms (FUN_009b2630(4)
    = 0x0135bf6c from the stopwatch calibration FUN_009b2360), FISTP, capped at 100 ms. The FILD saw ~1.4e17 (high
    dword = a stale object pointer), so every update got 100 ms: menus 3x real time at 30 fps, more at higher rates.
    Fix: accessors are may_alias again; what the distinct types bought (guest stores not reloading X86 register
    fields) is now `X86 *RECOMP_RESTRICT c` on every generated function (translate.py). Measured: APT gets 33 ms per
    update (990-1023 ms per s); the selected row's scrolling label 176-178 px/s at 2500 wide (0.24 row widths/s) at
    30 and at ~45 fps, the person's Windows recording 0.25 (dist/pics/6); before 551/860/973 px/s at 30/50/52 fps.
    Driving after the fix 90-114 shown fps on the bench (no loss). insns test added (mixed-width writes/reads; the
    small harness does not reproduce the old miscompile, the in-game measurement does).
  * How it was found (for next time): measure the animation against the person's video (bar widths per second), then
    NFSU_SUBSYS (main-loop stages: EMainUI factory 0x00669dd0, job vtable 0x00c0bc74 -> 0x00669d40 -> UI manager
    [0x013335ec], its +0x34 APT object, vtable 0x00ca4ef0, update 0x00a1cce0 -> FUN_00a1c960 frame time ->
    FUN_00a0d5d0 "nDeltaTimeMS"), NFSU_APTLOG (frame time per update + calibration), then a temporary fprintf in the
    generated chunk. Ruled out on the way: FSIN/CW/MINSD reverts, free threads, the main-loop clock (correct), RDTSC.
  * RDTSC is now real time at 1 GHz (monotonic ns, strictly increasing), not ms*10^6 + 1000 per read.
  * Menu frame rate (core.nfsu menu_fps, default 45 "as on the original PC"; 30/60/not limited): animations keep
    real time at any setting now. The limiter waits while the world's 60 Hz tick counter 0x00d3c108 (FUN_006a2980;
    0x00d3c100 = world seconds) has been still for 250 ms - pause menu over the city, frontend, videos, loading. The
    old signal (chase camera 004e0e30 idle) missed the pause menu, where that camera keeps ticking.
  * Main loop facts: FUN_006af1b0 busy-loops on QPC (dt ms, pass when > 0.25 ms) -> FUN_006ae4c0(dt*65536) ->
    seconds; FUN_006aded0 clamps to 4/60 s and counts 1/3600 s ticks. Stage descriptors at 0x00c06db0.. (28 bytes,
    name at +4, factory at +0xc): EMainAudio/IO/Min../Online/Render/Service/Simulation/Streamer/UI.
  * Diagnostics added: RECOMP_TRACE_INPUT (key/mouse edges, XInput packets, WM_KEY*/WM_MOUSE*), RECOMP_TRACE_TIME
    (=1 clock callers per 5 s, =2 also RDTSC), RECOMP_HOST_DUMP_AFTER=<s>, core.nfsu NFSU_SUBSYS=<s>, NFSU_DTLOG,
    NFSU_APTLOG, NFSU_MENU_FPS=<fps> (0 = not limited). Frame dumps at 3024x1898 are 17 MB: dump late and sparsely.
  * Testing trap: a test window takes keyboard focus; if the person plays their app at the same time, their key
    presses land in the test. Always pgrep first.
- 2026-11-16/17 (the person: Mac resolutions in the game's own menu, no stretching, F10 "maximum 960" and a
  screen-rate limit, "make it run at 960 without drops", "speed up the main thread"):
  * RESOLUTIONS in the game's Resolution spinner (pause > Настройка > Параметры изображения): core.nfsu rewrites
    both tables (k_res_rows, 14 rows incl. 1512x950/982, 1728x1080/1117, 3024x1900/1964, 3456x2160/2234; new rows
    hash MAC_HASH|i, captions "%ux%u" from a REPLACE hook on FUN_00586630). d3d9 display_modes() adds the 8 sizes.
  * WIDESCREEN (core.nfsu widescreen, default on): GetAspect FUN_0076b7f0 answers the real aspect; projection
    FUN_0074a710's sin/cos call sites get Hor+ angles (v fixed at 16:9's, h = atan(tan v * aspect)); HUD width
    0xc21b00, y-scale 0xc22620, height 0xc21f20 and matrix row 3 (FUN_0076cfd0) keep the HUD centred and unstretched.
  * F10 frame limits: rates {off,40,60,120,960 "maximum", screen refresh}.
  * BUILD: ThinLTO on the translated code (RECOMP_GEN_LTO, cache build/lto-cache) +10-20%; PGO (RECOMP_PGO_GENERATE=1
    build, run with LLVM_PROFILE_FILE=/tmp/pgo/%p.profraw, `xcrun llvm-profdata merge -o build/pgo/game.profdata`,
    rebuild). Re-collect after translator or native changes (re-collected 2026-11-17: +3% standing, more driving).
  * TRANSLATOR: accessors are one TBAA type (unsigned _BitInt(32)) for every access wider than a byte (64-bit and
    double as two halves), so widths alias each other (the APT bug) yet don't alias X86 fields; [translate]
    flags_across_blocks: function-wide flag liveness (dead after RET / before CALL). Stable section 61 -> 80 fps.
  * HOST d3d9: occlusion query results are fetched on the GPU thread and cached (no sync; RECOMP_D3D9_QUERY_SYNC=1).
    NOOVERWRITE locks go into the live Metal buffer (host_d9_buffer_append; RECOMP_D3D9_APPEND_COPY=1 restores the
    copy): the game appends to one 256 KB ring ~245 times a frame and each lock had copied the whole buffer into a
    new one - 60 MB a frame, 55% of the Metal thread. 79 -> 112 fps. Batch payload is a memcpy buffer (ByteBuf).
  * RUNTIME: D3DX pure math (D3DXVec*, Matrix...) takes a short import path (no checkpoint/observers;
    RECOMP_IMPORTS_PURE_SLOW=1); the Direct3D lock is an owner-word spin lock (std::recursive_mutex was 5% of the
    render thread). QPC counts real microseconds (it was ms*1000: the main loop's frame time came in whole ms, 5/6 ms
    steps at 170 fps); boot sets host_set_time_source_realtime(true), a fake/pinned clock keeps ms; one clock read
    per call (the render thread times thousands of calls a frame through FUN_004b6900); RECOMP_QPC_MS=1 = old.
  * NATIVE (native/nfsu_native.c, NFSU_NATIVE_CHECK=1 compares, NFSU_NATIVE_OFF=1 = translation only):
    FUN_0074a1d0's no-plane-set branch (85% of ~1M calls/s: bounding sphere projected through the view matrix
    0x00ea0390+(view-0x1f)*64, helpers 4572d0/410100/457390/457340/4b2420/4b2960 and the runtime's own
    D3DXVec3Transform via recomp_d3dx_vec3_transform) - 122.8M calls, 0 differences; 124 -> 170 fps standing.
    msvcr80 qsort (1003cee0) with the draw-key comparator FUN_00763970 (two u64 keys) as the listing's algorithm
    (median of 3, cutoff 8, 30-entry stack) - 280000 sorts byte-identical; +8%. Translate.cmake now gives the
    native header to aux modules too (mod hooks can't reach aux functions: recomp_index_of is main-module only).
  * RESULT (bench /tmp/bench2.sh, Bench0 profile, limit 960): standing section 61 -> ~177 game fps, driving
    ~200-230; driving game frames p50 4.5 ms, p99 5.9, p99.9 8.7. Now the game's RenderThread is the limit (main
    waits ~22% in FUN_00769460): its top costs are our Dev_DrawIndexedPrimitive (~10%), FUN_004b5130 (~10% self,
    timed by the game's own profiler wrapper 004b6900), import dispatch (~4%).
  * RENDER THREAD (2026-11-17 morning, "разогнать поток отрисовки"): FUN_0078aaa0 (draws one list item) formatted
    "eDataRender: %s" with the game's sprintf at 0078ab54 for every item - a dead profiler label (buffer [esp+0x24]..
    never read; FUN_004b6900's counters 0x00d86f68/6c write-only): native FN_004b6e60 skips that one call site
    (16% of the thread; 177 -> ~194 standing). FUN_00780390 (render-side on-screen sphere test, view matrix
    0x00ea0390+view*64, constants 0xd543d0/cc/c8, AL result, EAX upper bits = its frame's [S+0x30]) shares
    sphere_on_screen() with FUN_0074a1d0's branch: 16.6M calls checked, 0 differences. PGO re-collected.
    Bench now ~190-200 standing, driving 200-245, driving frames p99 5.5 ms. Both game threads are now about
    equally loaded (each waits ~12% for the other). Remaining render-thread cost is spread: material binding
    0x540a90-0x541980 (~12%, FUN_00767200 constant setters), Dev_DrawIndexedPrimitive ~10% (constants copy 2.4%:
    ~2.2 KB per draw, 3 MB a frame, already trimmed to the shader's max register). Main thread: scenery LOD/visibility
    FUN_0082a7d0 (10.6% self) and FUN_00825b70 (7%).
  * "I set 960 and still see 60-110" (2026-11-17): the overlay counts frames that reach the screen (<= 120 Hz);
    the game itself made 180-245 on the person's settings (profile copy BenchP = Bench0 save + their
    mod-settings/registry). The presenter lost refreshes too: with 3 submissions in flight 10-12% of frames stayed
    up two refreshes and sealed->screen was 32-34 ms (presenter waited ~4.5 ms per drawable). Default now 2 in
    flight (RECOMP_HOST_PRESENT_FRAMES=1/3): 960 limit 107 -> 115 shown, 12.5% -> 4.3% doubled, 23 ms; screen limit
    109 -> 117, 2.6%. Overlay (glass + Metal) adds "Game" fps (every sealed frame) and "Screen" Hz, and colours the
    big number against min(limit, refresh). /tmp/onscreen.py measures on-screen intervals from the CSV.
    TRAP: a run while the person uses the Mac (window covered) shows ~59 fps, 50% doubled - check HIDIdleTime
    (ioreg -c IOHIDSystem) and don't open test windows while they are active.
  * VSYNC (2026-11-17, "no vsync analogue; 114-118 at 120; 60 Hz MacBooks must work automatically"): F10 "screen,
    synced" (frame_limit 5, now the default for new profiles) = mods_display_vsync(): Dev_Present waits until the
    presenter took the previous frame (host_present_wait_taken; a free thread blocks on its condvar, a baton thread
    polls guest_sleep_ms(1)); the presenter paces at the window screen's refresh, re-read every tick, so 60/120 Hz
    and screen changes follow. RECOMP_PRESENT_SLOTS=1 = old slot pacer. Result: 0 dropped, 0 repeated when the game
    keeps up. The D3D9 Metal renderer has its own command queue (RECOMP_D3D9_SHARED_QUEUE=1 = shared); a MTLEvent
    orders each presented frame before the presenter's copy: drawable wait 2-4 ms -> 0.01 ms. The glass overlay
    (AppKit views over the Metal view) forced composition: now off unless RECOMP_PERF_GLASS=1; the Metal overlay
    (+ SCREEN HZ / VSYNC line, budget = min(limit, refresh)) is the default. Measured with the person's settings:
    on-screen doubles 5.9% -> 0.9% display-side; remaining misses are game frames over one refresh in heavy
    stretches (2-3% in some runs). Findings: presentedTime = submit + exactly 2 refreshes at any phase (ProMotion),
    so anchoring ticks to it gains nothing (RECOMP_PRESENT_LEAD, off); 3 in flight is worse (drawable starvation).
  * DIPS TO 60 UNDER VSYNC (2026-11-17 afternoon): (1) the person had F10 "120 FPS", which used the old slot pacer
    (846 dropped, 768 doubled a minute in their log): every fixed limit the presenter paces at now uses the vsync
    wait too and says ", synced" in F10. (2) The real cause: presentDrawable:afterMinimumDuration: made the
    ProMotion panel run at 60 Hz for seconds at a time (frames took 4 refreshes submit->screen; 12-15 of 110 s).
    Plain presentDrawable is the default now (RECOMP_PRESENT_MIN_DURATION=1 or HOST_PRESENT_PACING=duration for the
    hint): 0 seconds at 60 Hz, 117-119.6 shown/s, 0.4-1.7% doubled. Also: a CADisplayLink on the game view asks for
    the screen's maximum rate (metal_surface.mm request_full_refresh; RECOMP_PROMOTION_HINT=0 off); adaptive vsync
    depth (one more frame may queue for 1 s after a repeat; RECOMP_VSYNC_DEPTH=1/2) - depth 2 always did not help.
    NFSU_DTLOG now prints menu-paced passes. Measure: /tmp/sixty.py (seconds with the panel at 60).
  * RARE SHORT DIPS (2026-11-17 evening): (1) the menu pacer (core.nfsu menu_pace) switched on mid-drive when the
    world tick counter 0x00d3c108 stood still >250 ms - the game briefly waiting for streaming - and held the main
    loop (which also services EMainStreamer) to 45 passes/s for ~1.5 s: threshold now 2 s (g_menu_idle_ns,
    NFSU_MENU_IDLE_MS). (2) The free render thread took the baton back ~60x/s from its message pump (SendInput,
    then PeekMessageA; GetTickCount ~95x/s) and waited ~1.2 ms each for the main thread's 1 ms slice: SendInput has
    a do-nothing shim and is free_ok with GetTickCount; a free thread waiting in sched_free_leave makes the holder
    yield at its next checkpoint (g_free_waiting; RECOMP_SCHED_FREE_SLICE=1 = old): waits 365 -> 3 ms per 5 s.
    Result on the person's settings at 120 synced: no seconds below 100 passes, late game frames ~0%, 118.5-119
    shown/s; ~1% display-side doubles left (GPU-heavy frames). Diagnostics: RECOMP_TRACE_BATON=1,
    RECOMP_TRACE_LONG_SLEEP=1, NFSU_DTLOG now prints longest main pass and passes over 9 ms.
  * EXACT 120 / GPU (2026-11-17 evening): overlay rates are frames over the time they span (a 2 s count read
    119.5 at a steady 120). Metal System Trace (/tmp/benchgpu.sh; tables displayed-surfaces-interval has "Direct to
    Display", gpu-performance-state-intervals the GPU clock state): the game is already shown Direct to Display (525/526
    frames); the GPU ran ~90% at the Medium performance state; fragment work dominates (main scene pass ~1.8 ms of
    ~5 ms). Pass traffic (RECOMP_D3D9_STATS now prints passes, MSAA/depth MP loaded/stored and who ends MSAA passes):
    28 passes, 8 on the 2x MSAA main target, ~1.4 GB/frame of load/store. Every pass now gets the visibility buffer
    (a query no longer breaks the pass; RECOMP_D3D9_VIS_ON_QUERY=1 old) and a whole clear on the open target is an
    in-pass quad (RECOMP_D3D9_CLEAR_NEW_PASS=1 old): MSAA reloads 34.5 -> 23 MP/frame; still broken by StretchRect
    (2-3), present, target switches. The dev build's Info.plist is marked a game with Game Mode like the packaged app.
    Remaining ~0.6-1.3% frames shown a refresh late = GPU at medium clocks; next ideas: StretchRect without a pass
    break, depth DontCare at present when the next frame clears it.
  * MULTISAMPLE SAVING (2026-11-17): RECOMP_D3D9_PASS_LOG=<present> logs one frame's passes/draws/copies. The main
    2x targets end 6 times a frame and only 1-2 of those saved samples are ever loaded (the rest are followed by a
    clear or by a StretchRect that reads the resolve; a pass can't stay open across a copy). Store action of a
    multisampled attachment 0 is now MTLStoreActionUnknown, decided in end_pass from a per (texture, n-th end this
    frame) history: after 2 frames in a row of "next pass cleared", only the resolve is kept; a wrong guess rebuilds
    the samples from the resolve (blit aside + full-screen draw; msaa_rebuilds_). Measured: 2 saved / 4 resolve-only
    per frame; RECOMP_D3D9_MSAA_TEST_SKIP=1 forces the rebuild path (checked visually: correct); _SAVE_ALL=1 = old.
    GPU time per frame did not visibly drop (fragment-bound, medium clocks); on screen 119/s, 0.8% late.
  * More speed: native FUN_008251b0 (occluder vs sphere, 5 planes; 35M checks, 0 diffs): FUN_00825b70 7.2% -> 3.4%
    of the main thread. Vertex constants travel as a delta against the previous draw (sent_/current_ in
    RenderThread; RECOMP_D3D9_CONST_FULL=1 old, RECOMP_D3D9_CONST_CHECK=1 compares: 38M draws, 0 diffs): batch
    payload 3.6 -> 0.87 MB a frame. recomp_d3dx_vec3_transform moved to runtime/d3dx_math.cpp (mods_tests links
    the native code without the dx library). mods_tests has 167 failures with or without these changes (they run
    against this game's settings layout), the --mods fixture needs another game's files.
  * Tests: --native all pass except pad_tests joystick name (pre-existing, files from 11-12) and host_boundary_check
    (3 pre-existing .mm files outside gpu/metal). host_tests' mailbox test updated to the oldest-first rule.
  * PICTURE (2026-11-17, F10 settings in mods/core/nfsu/mod.toml): colour_filter (restart) 0 as in the game,
    1 softer, 2 neutral - a BEFORE hook on FUN_00766f40 (ecx = owner; cube data at [owner+4+4k], k 0..8, and
    [owner+0x5c]; 3 bytes a texel at data + 3*((z*32+y)*32+x), byte 2 = R(x), 1 = G(y), 0 = B(z)) moves the raw
    cube bytes toward x*255/31 once per data pointer. Changing only the filled textures (hook on FUN_00766db0)
    changed nothing on screen: the frame's blended cube comes from the raw data. The cubes were 16-92/255 off
    neutral on average; measured on the same frame: road (113,104,82) -> (85,82,72), sky (198,225,221) ->
    (159,218,217). NFSU_COLOUR_FILTER=0/1/2 overrides, NFSU_LUT_LOG=1 logs each cube. FUN_00766db0 is called by
    D3DXFillVolumeTexture through guest_call, whose return sentinel longjmps out of a REPLACE hook after
    call_next (the hook frame leaks: "hook recursion deeper than 32") - hook such callbacks with hook_return only.
    sharpening (live) 0..3 -> 0/0.35/0.65/1 read by the presenter -> compositor_set_sharpening: Metal-only fragment
    compositor_sharp_fragment (CAS-style, 4 neighbours, buffer(1).x strength) for the main picture only, after
    MetalFX; "[host] compositor: sharpening..." logged once; other backends draw it plain. Frame dumps are taken
    before the compositor (no sharpening in them); /tmp/pic.sh TAG TINT SHARP (DA/DE/DUR env) runs a BenchP copy.
    Waiting for the person to be away: HIDIdleTime >= 30 s (their choice), and pgrep with "Undercover[R]ecomp" so
    the waiting shell never matches itself (it did, and the wait never ended).
  * MOTION BLUR (2026-11-17, setting motion_blur, restart; 0 smooth default, 1 as in the game, 2 smooth 0.6x):
    FUN_00798360 draws DrawMotionBlurQuad (FUN_0077df40, stdcall owner/target/strength, RET 0xc) twice, strengths
    0.5 and 1.4; the pixel shader is an 8-tap blur along a per-corner vector (zero mid-screen). 0.5 and 1.4 leave
    gaps -> ~15 stepped copies at the edges at speed. A BEFORE hook rewrites the strength so pass 1 = total/9,
    pass 2 = total*8/9 (same length, 64 even taps): verified on frame dumps at 101-119 km/h.
  * HANG ON DISPLAY SLEEP / MINIMISE (2026-11-17): WM_ACTIVATEAPP(0) sets [0x00df2e48]; the pump FUN_0074be20 (on
    the render thread) then blocks in GetMessageA while [0x00df9a94] is set. The host hands queued input (the
    reactivation) over only from main-thread clock reads or when no hook frame is live; the main thread waits for
    the render thread in FUN_009a9f80 Sleep loops inside hooked functions -> deadlock, force-quit needed
    (reproduced with `pmset displaysleepnow`, sample). Fix in core.nfsu (always on): the MESSAGE_PUMP hook clears
    [0x00df9a94] for the call while inactive (drains with PeekMessageA, naps 16 ms via guest Sleep), and a BEFORE
    hook on FUN_009a9f80 reads timeGetTime (IAT 0x00bd65e4) while inactive so the host delivers the queued
    WM_ACTIVATEAPP(1). Verified: display sleep 10 s and switching to Finder 10 s both pause and then resume, rc=0.
    /tmp/hang.sh (AX minimise - didn't minimise the borderless window), hang2.sh (display sleep), hang3.sh (app switch).
  * FOUR POINTS (2026-11-17 evening; the person: soap, notch/bands, shadows, F10 menu and limiter):
    - LIMITER: 40/60 were synced and only waited for the presenter to take the frame (120/s). Vertical sync is
      now its own switch (host.display/vsync, default on; F10 row SRC_VSYNC after the frame limit); with it on, a
      limit below the refresh first waits for its slot (SlotPacer at k refreshes) then for the screen; off, the
      limit paces alone. Measured: 40 -> 40.00 fps, 0 dropped (vsync); 60 vsync off -> 60.00.
    - F10 MENU: the wheel scrolls the list (g_scroll_rows, 2 rows a notch; the view follows the selection only when
      the selection changes) instead of moving the selection, which the pointer took back. kit/mods/menu_lang.cpp:
      Russian when the registry's Language says Russian (RECOMP_MENU_LANG=ru/en overrides; RECOMP_MENU_TR_LOG=1 lists
      untranslated strings): kit strings in a table, mod strings from manifest label_ru/help_ru/choices_ru;
      numbers by pattern. MenuLine.switch_on keeps switches independent of "On"/"Off". Panel 1480x1010 of 1920x1080,
      help up to 6 lines. RECOMP_MENU_DUMP=<dir> writes each new menu page as PPM (no screen capture needed).
      core.nfsu's [settings] are generated: tools/dev/gen_nfsu_settings.py tools/dev/nfsu_settings_table.py.
    - SOAP: composite_blur (FUN_007936f0, end of FUN_00798360) is out = lerp(picture, half-size blur image,
      cvBlurParams.x = [effect+4]) on every pixel; while driving that weight reaches 1.0, so the whole picture,
      centre included, was the half-resolution copy. Kit RECOMP_D3D9_SHADER_PATCH_DIR: <dir>/<key>.patch text
      patches one translated shader (@@FIND/@@REPLACE/@@END); mods/core/nfsu/shaders/cf5742848bdb4cf1.patch weights
      the composite by distance from the centre (centre sharp, edges as before); launch.c sets the dir unless
      motion_blur = 1. motion_blur: Smooth / As in the game / Lighter (vectors and weight 0.6) / Off (both 0;
      FUN_00798360 REPLACE hook scales [effect+4] for the call). Depth of field (depth_of_field, live, default
      Weaker): "vt_enhanced" chosen when FUN_0077bf50 (RET 4) sees blur > 0.001; Weaker halves FUN_0077c000's
      near/far blur, Off answers 0 from FUN_0077bf50. VisualTreatment's glow field [fx+0x114] was 0 (glow scaling
      kept only as NFSU_GLOW A/B). CTABs of ShadersLibrary.bin read with a small parser (c1 = cvVisualTreatmentParams).
    - NOTCH: macOS's Space full screen puts the window below the notch (1512x949 pt at y 33). Kit
      RECOMP_MAC_FULLSCREEN_NOTCH=1 sets SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=0 (SDL's own borderless full screen over
      the whole frame: 3024x1964 at 0,0); launch.c from core.nfsu/notch_area (default on). The person must pick
      3024x1964 in the game's menu (their registry had 1900). "[host] drawable ..." now logs window/safe area.
    - BANDS: menus/map/loading are drawn for 720 units; the 16:10 HUD canvas is 1280x804 -> 42-unit bands. hud_fill:
      while the world tick 0x00d3c108 has stood still 300 ms the canvas is 1280x720 (stretched ~12% taller in menus),
      driving keeps 1280x804. 720xaspect wide (1146) cut the speedometer: the game fixes HUD places at load.
    - SHADOWS: kit RECOMP_D3D9_TARGET_SCALE="WxH:F,..." makes targets of exactly WxH F times larger (t.scale, the
      viewport follows); launch.c sets "3072x1024:2,256x512:2" (CSM cascades, reflection maps) unless
      core.nfsu/sharp_shadows = 0. RECOMP_D3D9_TARGET_LOG=1 lists targets (also 512x512, 756x474, 1512x950/756x475).
    - mods_tests: 171 failures here (167 before, the difference in path-dependent replay/run-record tests), none in
      the menu or display tests.
  * FIVE POINTS (2026-11-17 night; after the person played):
    - NOTCH PLACEMENT: host_present_game_rect (present_pixels.cpp) now also places landscape pictures: with a top
      strip (safe_top, 64 px in notch full screen) a picture shaped nearer the area below the strip goes there
      (3024x1900 -> y 64, exact), one shaped like the whole screen fills it (3024x1964, 3456x2234). Test
      test_game_rect_landscape_notch. d3d9 display_modes offers the fixed 14"/16" Mac list only while this screen's
      size is unknown (the other Mac's sizes were only squeezed and showed half the notch).
    - HUD PROPORTIONS: the hud_fill layout switch leaked into driving (HUD built at load with 720 units: minimap 15%
      tall at 3024x1964). Now the layout globals always follow the real aspect; menus are stretched to the full
      height only through the per-frame HUD matrix (hud_matrix_leave: y scale -(2+2d)/720, ty 1+d, d = one pixel)
      while the world tick is still (300 ms). Verified: round minimap/speedometer driving, no bands in the pause menu.
    - F10: mixer_free, render_free, mac_resolutions, widescreen are no longer rows (always on; launch.c ignores saved
      mixer/render values). New rows gloss (launch.c -> kit RECOMP_D3D9_PS_CONST_SCALE "name:factor,...": named
      pixel-shader constants from the shader's CTAB scaled per draw - cvWorldSpecularColour, cvSpecularMin/Range
      0.6, cvEnvmapMin/Range 0.75 for Softer, the default; d3dx9_ctab_float_register). On a sunny frame the visible
      change was small (car paint reflections); the person's wet-road look (tunnel, lights) was not reproduced.
    - SMOKE/DUST: kit RECOMP_D3D9_TARGET_SCALE "1/4:2" = targets a quarter of the back buffer's width (756x474 smoke
      and depth, 756x475 blur at 3024x1900) at 2x; launch.c adds it to sharp_shadows.
    - HEAT: consecutive frames all differ at 120 (no duplicate renders to drop). GPU traffic: depth stored 83 MP vs
      read 47 per frame. Depth store saving in d3d9_metal.mm: per depth buffer - one whose contents nothing read in
      its first 240 ends gets MTLStoreActionDontCare (Unknown at pass creation, decided in end_pass); the first read
      (a pass loading it, a depth StretchRect, a shader sampling it) makes it store for good (depth_burned_). A
      per-(texture, n-th end) guess like MSAA's saved more but guessed wrong ~1 per 2.5 s while driving (pass counts
      vary) - dropped. RECOMP_D3D9_DEPTH_SAVE_ALL=1 = old. Stats line "depth ends per frame". GPU ~5.1 ms/frame
      at 120 with 2x shadows+smoke (unchanged from before them). RECOMP_HOST_DUMP_LIMIT=<n> caps frame dumps.
  * SIX MORE (2026-11-17 late night):
    - HUD: FUN_0076cfd0 builds the HUD matrix only when the HUD is built (loads), so a menu stretch written there
      stayed in driving. core.nfsu remembers the built (driving) x/y scale and translation and hud_apply writes the
      right ones every main-loop pass (MAIN_LOOP_STEP hook). Menus/loads/pause fill the height UNDISTORTED: uniform
      scale 720 units -> height, the 1280-unit layout centred, (1280 - 720 x aspect)/2 units cut at each side (the EA
      logo was a 1.16 oval). Measured: minimap radius right 194 / down 192 at 3024x1900 and 3024x1964, also after a
      pause; save menu title not cut. /tmp/ring.py measures it.
    - STORE DECISIONS: d3d9_host.cpp host_d9_next_use(tex, as_depth, viewport) scans the commands still to run (the
      batch executing and the queued ones, under m_) for the next thing done to a target: whole clear (1), read (2:
      a draw rendering to it, a depth sampled, a copy into it / from a depth, a readback), unknown (0: batch end or a
      Call). The executor runs one batch behind (a batch starts when the next is queued or when sync/run_on_pump set
      drain_; RECOMP_D3D9_QUEUE_LAG=0 = old). Depth: stored unless the commands say "cleared next" - never guessed;
      measured 0 of 10 depth ends skippable (next use is in the next frame). MSAA: the commands first; when they
      don't say, the frame history as before (a wrong guess is drawn back from the resolve: correct picture).
    - SMOKE: setting smoke_resolution (0 quarter, 1 half, 2 full - default; launch.c builds RECOMP_D3D9_TARGET_SCALE
      "...,1/4:2|1/4:4"). Full costs GPU 5.2 -> ~8 ms/frame at 120 (dips to 113 fps).
    - GLOSS live: kit RECOMP_D3D9_PS_CONST_SETTING=<core key>: factors are the strongest level, level L of N applies
      1-(1-f)L/N, read every 100000 draws. motion_blur, menu_fps, hud_fill also live (core.nfsu re-reads them; the
      blur shader patch is always on). Sharpening: levels 0.5/0.8/1.0 and the CAS difference x(1+strength).
    - SOFT SHADOWS: not done. The game reads its R32F shadow map with 4 point taps and blends the compare results by
      fract() itself (no hardware compare); scaling cvShadowParams x2 made edges blockier. Needs a shader rewrite.
  * SOFT SHADOWS + SMOKE (2026-11-17, last): kit d3d9_shader.cpp reads the CTAB sampler named by
    RECOMP_D3D9_SOFT_SHADOW_SAMPLER (Program.soft_shadow_sampler); d3d9_shadergen.cpp (MSL pixel only) follows each
    temp component: a read from that sampler (coords kept in shuvN), mov copies, add with the surface depth (kept in
    shzN, sign from the read's negate), and turns the cmp of that difference into mix(src2, src1, d9_soft_shadow(...)):
    centre + 8 reads on a disc whose radius grows with the average gap to occluders (RECOMP_D3D9_SOFT_SHADOW="K,MAX",
    launch.c 600,5), one lookup per surface depth (cache keyed by the value's register/component; the game's own
    4-read blend then averages equal values). 31 game shaders use it; all compile. Shadow map 1.5x (TARGET_SCALE
    3072x1024:1.5); GPU 5.0-5.6 ms/frame at 120 (unchanged). Setting soft_shadows replaces sharp_shadows.
    Smoke: pass log now prints each draw's ps/vs key, the shader dump prints "dump key = code key". The smoke particle
    shader (code 3cf6a4bed07ac3a7, dump eb3cd4522095ae11) took its colour from the vertex colour only: patch
    mods/core/nfsu/shaders/eb3cd4522095ae11.patch shades denser parts darker, wisps lighter, texture luminance and a
    smooth value noise. smoke_resolution default 1 (half). A 4 s game-side dip to ~57 fps appeared in 2 of the last
    benches (GPU time normal; many benches in a row - heat or streaming, not settled).
  * HITCHES, BLACK PEDESTRIANS, HEAT, LOADS (2026-11-17, latest; the person's log: 80-180 ms first-time shader
    compiles, "a pixel shader is not translated: source register type 17" at an arrest, GPU 105 C):
    - vPos/vFace: d3d9_shadergen.cpp translates R_MISC (type 17) in pixel shaders: index 0 = vPos (fragment
      position - 0.5), index 1 = vFace (+1/-1 from [[front_facing]], uses_face adds the argument). The arrest
      cutscene's pedestrian shaders were skipped before (black). Not yet seen in an arrest by me.
    - Shader compiles off the frame: d3d9_metal.mm function() compiles unseen shaders with
      newLibraryWithSource:completionHandler: (PendingLib); until done the draw is skipped and nothing is cached
      (a just-appearing object misses a frame or two instead of the whole frame waiting 80-180 ms). Log "N new
      shaders compiled in the background so far (no frame held)"; RECOMP_D3D9_SHADER_ASYNC=0 = old. ShaderWarm still
      compiles the recorded ones at start.
    - Shadows lighter: setting shadow_softness (0 off, 1 light "300,3", 2 medium "600,5" default, 3 strong
      "1200,8"; restart) replaces soft_shadows; the shadow map is no longer enlarged (1x). smoke_resolution default
      back to the game's quarter (0).
    - LOADS: the menu limit (menu_pace) held loading screens to 45 passes/s too, and the loader advances once per
      pass: Enter -> driving 4.5 s paced vs 2.5 s unpaced (RECOMP_PROFILE_EVERY=1 runs, BenchP). Kit
      recomp_file_bytes_read() (kernel32.cpp ReadFile + MapViewOfFile; exported with used/visibility default -
      the linker dead-stripped it and the mod's weak reference was NULL); core.nfsu loading_now(): > 256 KB read in
      a 100 ms window = loading for 500 ms -> unpaced. Menus stream ~22 KB/100 ms (stay paced); the warning screen
      reads ~3.4 MB/s (now unpaced). NFSU_READLOG=1 logs the windows, NFSU_PACE_LOADS=1 = old. Measured: load at
      102-111 fps, driving ~1.5-2 s sooner; menus 45.
    - Trap: one run stayed windowed 1280x960 (never went full screen, 58 fps) - the person was at the Mac; the
      repeat was normal. Dev runs (run.sh) get Space full screen (no notch env; that is launch.c's).
  * NIGHT 2026-11-18 (the person asleep, YouTube playing in Chrome: test runs silent with SDL_AUDIO_DRIVER=dummy and
    Chrome activated after each - /tmp/dev.sh, /tmp/heat.sh, /tmp/tput.sh; screenshots in dist/pics/11):
    - ROAD SPECKLES / BLACK BLOTCHES ON PEOPLE AT AN ARREST: soft-shadow self-shading. d9_soft_shadow compared every
      disc read (up to 5 texels away) with the centre's depth; under a low sun (sunset, I-20) a sloped surface shaded
      itself in patches. Now a receiver-plane bias: each read's comparison depth follows the surface plane in
      shadow-map space (d9_soft_slope from dfdx/dfdy of uv and z, clamped 0.05/texel). Measured on the same daytime
      frame vs no soft shadows: extra dark pixels 31-34k -> 6-7.5k (the rest moving cars); the whole SPEEDHUNTERS
      banner had been covered. The arrest scene itself was not reproduced. Tried: 4+4 gather() fetches instead of 13
      reads - slower (6.7 vs 6.3 ms GPU), reverted. Soft shadows cost ~0.8 ms GPU/frame (~14%) at 4x 3024x1964.
    - EA NATION: after a save loads, start-flow step FUN_005e7b40 (fastcall, step [ecx+4]) at 0xb creates the PC
      sign-in (FUN_00750310(2) "FEOnlineLoginStateManager FEPCOnlineLoginStateManager", child step 0xd): "Подключение к
      серверам EA Nation", then FUN_005b1830 state 42 -> FUN_005b01e0 "Не удалось установить связь с EA Nation" waiting
      for Enter. Setting ea_nation_signin (default off): REPLACE hook runs the function's common prologue (owner
      [ecx+0x58] vtable+0xc; FUN_00544fd0(8,0) + FUN_00669720) and the step's own way on, vtable +0x180 (what case 0xb
      does with nothing to sign in with). Verified: straight to driving, one Enter fewer. NFSU_DIALOG_LOG=1 logs every
      message box (FUN_005a3be0/005a3d80) with its text, the front-end state and the sign-in object.
    - PIPELINES in the background: d3d9_metal.mm pipeline() makes new ones with a completion handler, waits <= 2 ms,
      else the draw is skipped this frame (the person's log: 38-67 ms holds). RECOMP_D3D9_PIPELINE_ASYNC=0 = old.
      ("untranslated shader" skips in D3D9_STATS include draws whose shader is still compiling.)
    - HEAT, CPU: the main thread waits for the render thread with `while ([0x00ea0164]==1) SleepEx(0)` (FUN_00769390 /
      00769460) - ~35% of every frame spinning a core flat out. Kit sched_sleep_ms: a Sleep(0) within 200 us of the
      thread's previous one is a wait loop and naps 30 us (baton thread: guest_yield first, then a plain host sleep with
      the baton kept unless a free thread waits; blocking in the scheduler instead ran the host idle waiter's event pump
      and cost as much as the spin). RECOMP_SLEEP0_NAP_US=0 = old. Game CPU 147% -> 98% at the same 119 fps.
    - HEAT, memory: (1) the Metal backend copied every presented frame into a shared texture (24 MB at 3024x1964, ~3
      GB/s) that only the smoke host reads - now host_d9_keep_presented(1) asks for it (smoke_main), else no copy;
      RECOMP_D3D9_KEEP_PRESENTED=1 forces. (2) Deferred clears: a multisampled pass that only clears and draws nothing
      (the frame's first pass on the 4x back buffer + depth) stores no samples/depth; the texture remembers the clear
      (Tex::pc_valid) and its next loading pass starts with that clear (resolve still stored). Per frame: MSAA colour
      loaded 47.5 -> 23.8 MP, stored 48 -> 23.8, depth stored 149 -> 124.5. RECOMP_D3D9_DEFER_CLEAR=0 = old. Stats now
      count what is really loaded/stored. GPU time barely moved (~0.2 ms): the GPU is shading-bound, not memory-bound.
    - GPU per pass (Metal System Trace, RECOMP_D3D9_LABEL_PASSES=1 labels encoders "c<id> WxH xS d<id>", /tmp/mstparse.py
      sums metal-gpu-intervals): main scene c5 (4x) 41%, c8 (4x) 19%, final pass on the 4x back buffer 11%, half-size
      smoke/glow ~14%, shadow map ~3%. The person's settings: FSAA 2 = 4x, 3024x1964; GPU ~6.3 ms/frame driving.
    - LOADING: loading screens were held to 120 frames/s by vsync and the loader advances once per pass. Kit
      recomp_present_unpaced_for(ms) (d3d9.cpp, exported; RECOMP_PRESENT_LOADS_PACED=1 = old) drops the limit and the
      screen wait; core.nfsu asks for 300 ms at a time while the world tick has stood 250 ms and the game reads over 2 MB
      per 100 ms (fast_reads: a scene load reads 10-22 MB/100 ms, the warning screen's video 0.34, menus 0.02).
      Enter -> driving ~1 s sooner (3.5 -> ~2.5 s). The save itself loads in < 1 s. FUN_006a3060 (cubic spline, SSE
      double + x87) is 19% of the main thread in a load's heaviest second - a native copy would save ~0.2 s; not done.
    - TRAP: from ~01:20 the test window got 60 Hz (59 fps under vsync, the presenter dropping frames) in every run,
      old and new code alike - the screen with the Chrome video; benchmarks after that use vsync off + 960 limit and
      count game frames (/tmp/tput.sh; /tmp/gfps.py).
  * MORNING 2026-11-18 (the person: spots still there, "foil" people, heat, loads, stretched menus):
    - ROAD SPOTS - REAL CAUSE (the night's soft-shadow fix was beside the point: same spots with shadows off and
      gloss as in the game). Found by skipping pixel shaders (kit RECOMP_D3D9_SKIP_PS=<hex,...>) on the person's I-20
      sunset spot (profile copy "NFS Undercover PQ" = theirs with only save "ru"): road shader code f9f24437ec03c9cf
      (dump f898acd9e2587422) adds a dual-paraboloid reflection from render target 14/20 (256x512, 4 levels; top
      half = up hemisphere). The game builds levels 1-3 by drawing level n from level n-1 OF THE SAME TEXTURE; the
      Metal backend refused a sampled texture equal to the pass target (feedback -> blank), so the blurred levels were
      garbage and grazing reflections came out as black/white blocks. Fix (d3d9_metal.mm levels_above): a texture
      view of levels 0..n-1 when the pass draws level n > 0; RECOMP_D3D9_LEVEL_VIEW=0 = old. Verified: blocks ->
      smooth sheen. The character shader (arrest people) samples tx0 with an explicit level too - the "foil" is very
      likely the same garbage; the arrest scene was not reproduced.
      Diagnostics added: RECOMP_D3D9_TEX_LOG_PS=<key> (textures/samplers of one shader), pass log prints each draw's
      viewport/level/scissor/cull, clears and feedback reads; RECOMP_D3D9_POINT_TO_LINEAR=1; core.nfsu
      NFSU_ASPECT_LOG=1 (GetAspect callers: 007454e5 IsWidescreen, 0074a7d5/0074a811 projection only).
      Shader debugging: a patch dir copy (/tmp/pdir) whose <dump key>.patch replaces "float4 col = oc[0];" to show
      intermediates as colour.
    - MENU PROPORTIONS: measured right. The save screen at 1920x1080 (profile "NFS Undercover P169", where the
      widescreen code changes nothing) has its panel at 330 x 303 units, the same as at 3024x1964 with the menu fill;
      the EA ring is 999x999 px (the person's oval screenshot was from before the 11-17 fix). The phone video of the
      Windows laptop (dist/pics/6) is too keystoned for 10% measurements - it misled me into mapping 1280/aspect
      units to the height (menus 15% flat, loading backgrounds short of the bottom): reverted. The "jump" between a
      menu and driving is the zoom (menus H/720 px per unit, driving H/831): F10 hud_fill off removes it with bands.
    - HALF RESOLUTIONS: 1512x982 and 1512x950 are in the game's list on this Mac (adapter modes + k_res_rows). They
      lighten the GPU only with render_scale "game resolution" (the person had "Screen", which draws 3D at Retina
      size anyway): throughput 2.0 vs 6.3 ms GPU/frame (vsync numbers mislead: the GPU lowers its clock).
    - START-UP: the only start-up movie is MOVIES\EA_Bumper_english_ntsc.vp6 (EA/Black Box/NFS, ~9 s). Kit
      RECOMP_FILE_HIDE="part|..." (kernel32 CreateFile: hidden names are "not found"); setting startup_movie (default
      off) -> launch.c hides "EA_Bumper_". The game then goes straight to the warning and title screens (title ~9 s
      sooner); checked in the packaged app.
  * 2026-11-18 noon (the person: keep the logo movie, make the LOADING itself fast; 1.5x drops fps, Screen = Game
    resolution heats the same; smooth the menu zoom):
    - Logo movie back ON by default: setting renamed startup_movie -> startup_logos (default true) so the "off" the F10
      menu had saved as the old default is ignored; launch.c hides EA_Bumper_ only when it is off.
    - Menu zoom is now a 0.32 s eased zoom between the driving and the menu matrix (hud_apply; NFSU_HUD_ZOOM_MS=0 =
      old jump); a forced re-apply (aspect/setting change) snaps.
    - render_scale 3 "Half the screen (cooler)": launch.c RECOMP_D3D9_SCALE=0.5; the Metal backend now accepts 0.5..8
      and below 1 scales only screen-sized targets (the frame and its 1/2 and 1/4 copies; shadow and reflection maps
      keep their size); the presenter enlarges with MetalFX. Frame checked (1512x982, HUD right). Screen = Game
      resolution when the game is set to the screen's size (the person's case), hence no difference for them.
    - Scene load after Enter (~3.5 s), per second: the menu's closing animation (main thread 32% busy), the loading
      screen pass-bound at ~209 fps (render thread waits on the GPU in Present, the main thread on it), one second of
      the game's own single-threaded load work (FUN_006a3060 spline 15%), then the fade-in. Not made faster this
      time; ideas: native FUN_006a3060, cheaper loading-screen frames, faster APT clock during the fades.
    - Remaining roadside "patches" after the mip fix: the game leaves the lower hemisphere of reflection map 14
      black; downward reflections (bumpy normals) hit it. Same at 0.5 and 1x; not changed.
  * 2026-11-18 afternoon (the person: speed up the load's own single-threaded work, no animation faster):
    - Measured without the profiler (RECOMP_PROFILE's recomp_call skews it): RECOMP_SLEEP_TRACE=<csv> (main-thread
      Sleep calls; columns stack12/stack20 added = the callers one and two wrapper levels up) and /tmp/loadwin.py
      (longest stretch with no main-thread Sleep after 55 s = the scene load's CPU work). Profile "NFS Undercover PQ",
      Enters 32/45/60: menu close animation 60.5..61.4 (main waits for render, untouched), then the CPU stretch, then
      ~60 ms of FUN_006af050's 8 ms minimum frame (Sleep(1) loop, the game's own pacing, left) and 1-2 menu-pacer
      sleeps. Where the stretch goes: `sample PID 2 1` at process age 61 s + /tmp/stree2.py (busy-only inclusive).
    - CPU stretch 630 -> ~390 ms (pairs of runs, NFSU_SPLINE_OFF/ATTR_OFF/SHIFT_OFF = the translation) with new byte-
      exact natives (native/nfsu_native.c; NFSU_NATIVE_CHECK=1: 0 differences in 4M spline, 6M+3M attribute, 4000
      shift-point, 0.8M clamped-curve, 3M curve calls): FUN_006a3060 cubic spline (x87 ops through fx87 = the
      precision-control field), FUN_00706380 / FUN_007063c0 its curve wrappers, FUN_00463280 / FUN_004636d0 the
      attribute hash lookups, FUN_006cea20 a car's shift points (the biggest: -115 ms; it calls the spline ~1000x a
      gear). Left in the stretch (busy inclusive): 0083bec0 -> 00a1cce0 -> 009e6b90 ~20%, car setup 006f4240 ~19%,
      00669d40/00679860 ~17%, all diffuse translated code.
  * 2026-11-18 HDR + the person's defaults:
    - Defaults: tools/setup_kit/defaults/mod-settings.json = the person's F10 settings as saved in the dist app
      (profile "NFS Undercover", 12:24; the stale startup_movie key dropped) + hdr_output 1, hdr_highlights 35. A
      new profile starts with them (launch.c copies Defaults once); mod.toml's own defaults were not changed.
    - Is there light above white to show? No: every target is BGRA8 (shadow map R32F); with them kept as RGBA16F
      (RECOMP_D3D9_FLOAT_SCENE=1, reads clamped as 8 bits stored them, PixelVariant::sat_mask) the scene and the
      whole post chain stay <= 1.0 with the sun in view. Unclamped float reads gave up to 7x - feedback across
      frames, not light. FLOAT_SCENE is kept as an experiment only (+1.2 ms GPU).
    - HDR v1 (alpha mask on an 8-bit back buffer + the MW knee expansion) was a disaster in the person's hands
      (dist/pics/13): overlays drawn after the composite with alpha (the sun's additive glare) cut the mask in
      their shapes - round blobs, tree silhouettes, a half-screen split -, the x6 expansion blew skies and white
      paint flat, the in-game menu was lifted too.
    - HDR v2 (2026-11-18 afternoon): RECOMP_D3D9_HDR_SCENE=1 (launch.c when core.nfsu/hdr_output) keeps the back
      buffer (first render target) as RGBA16Float. Every draw into it is clamped as 8 bits would store it
      (PixelVariant::out_sat). The scene's composite - the draw into it sampling a screen-sized target (ps
      823d0a95d7df282e) - writes alpha 2.0 (hdr_stage) only while the core mod says the scene is live
      (recomp_hdr_scene_live from menu_pace: world ticked < 250 ms ago; menus and pauses stop it) and the presenter
      outputs HDR (g_host_hdr_output_on). Later non-additive draws (HUD, text, minimap) clear the mark with alpha
      ZERO,INVSRCALPHA; additive ones (dest factor ONE) keep it. The presenter (compositor_hdr_fragment mode 3)
      shows min(rgb,1) - exactly the 8-bit picture - and where the mark survives lifts near-white by the
      brightest channel y: y + 41.67 (y-0.88)^2 (white -> 1.6 encoded), the part above 1.0 scaled so white
      reaches the core mod's hdr_peak (new key, default 15% of the EDR headroom: x3.25 on the MacBook; the old
      hdr_highlights 35 the person saved is ignored). MetalFX (HDR mode for float input) and sharpening are kept.
      Checked by dumping the float back buffer at pass-log frames and simulating the presenter (/tmp/hsim.py, in
      tools/dev/perf): SDR part identical, HUD and text unmarked, lift only on bright clouds and glints, menus and
      pause unlifted. Not seen on the screen (locked while testing). GPU 6.2 -> 6.8 ms at full resolution.
    - v2 fixes (report: a thin glowing outline round leaves; colours dull next to the sun): the lift is now
      measured on the frame as rendered (texture(1), linear) and on the shown colour, the smaller of the two - the
      sharpening's and MetalFX's light fringe along dark edges had been lifted into the outline; MetalFX stays in
      Perceptual mode for the float frame (the HDR mode drew its own fringes). The HDR drawable now uses the
      screen's own colour space linearised (CGColorSpaceCreateExtendedLinearized of the window's NSScreen) instead
      of extended linear sRGB: the SDR drawable has none, so its values reach the MacBook panel in P3, and sRGB
      had squeezed them into the smaller gamut. Same-moment captures (race start, /tmp/pkgrun.sh): leaf edges
      identical with and without HDR, colours as in SDR. The dark-light outline both show is the person's
      "Strong" sharpening.
    - Next report (colours dull next to the sun; traffic cars plastic): (1) core.nfsu/hdr_world (default 130%):
      the marked scene is shown that much brighter than SDR white (CompositorInput::hdr_scene_gain, passed in the
      Hdr knee slot for mode 3), with saturation 1 + 0.25 (gain - 1) on BT.2020 luma; HUD and menus stay at 1;
      capped so gain x peak fits the EDR potential. (2) The gloss setting scaled cvSpecularMin/Range and
      cvEnvmapMin/Range too - those exist only in the car-paint shaders (cvVinylScales, ENVMAP_PARABOLOID1), so
      "Matte" (the person's) made car paint dull plastic. launch.c now scales cvWorldSpecularColour only (roads,
      buildings, terrain, the PCA shader); the row is "Gloss of roads and buildings". RECOMP_D3D9_PS_CONST_LOG=1
      prints each scaled shader's CTAB names (kit d3dx9_ctab_names).
    - Next report (HDR glints flicker at speed; the cop-detection animation too bright; unnatural blur, halos round
      lights, sky tint on leaf edges; bluish smoke):
      * HDR lift brightness = average of 9 taps (~5 px) of the un-enlarged frame, still min'd with the shown pixel.
      * Everything drawn into the back buffer after the composite is the HUD (197cb60e.../91a2167f..., additive
        ones too: the composite holds the sun's glare), so additive draws now clear the mark as well.
      * The composites 823d0a95d7df282e / 7f36120e05dc47e4 (patches named by CODE key - d3d9_metal.mm now also
        reads <code key>.patch, so every variant is patched): the HDR mark is lowered by MISCMAP2's screen
        overlays (cvVisualTreatmentParams2/3 .z/.w) and by 7f36's cvVisualEffectFadeColour.w - the game's flashes and
        fades are shown as drawn. Not seen with cops in a test.
      * Fog/haze pass 961f166f1ae442c4: TEXTURE1 is the SHARP scene, DIFFUSE the quarter blurred copy (a first patch
        that swapped them blurred the whole screen - checked by capture, reverted); its final blend toward the
        blurred copy by alpha x c9.w is patched to 0 - distance smear and bleeding gone, fog kept. Captures: far road
        and signs sharp, traffic headlights with little halo.
      * Bloom: new setting core.nfsu/bloom (0 game, 1 weaker default, 2 off) -> PS_CONST_SCALE rule
        "cvVisualTreatmentParams:0@bloom" (kit: "name:factor@setting" = a rule with its own setting).
      * Smoke patch eb3cd4522095ae11: colour 35% saturation toward its grey (the vertex colour's sky blue).
    - Diagnostics: RECOMP_D3D9_PASS_LOG takes a list ("3700,4300"); each such frame writes /tmp/backbuffer-N.raw
      (RGBA halves for an HDR back buffer) and /tmp/float-target-N.raw; begin lines show the MTLPixelFormat, draw
      lines the blend state and HDR variant. /tmp/pkgrun.sh TAG SECONDS "shot ages" [env]: the dist app on PQ's
      save with the person's mod-settings, screen captures (black while the Mac is locked).
  * Profiling recipe (copies in tools/dev/perf/; they expect to live in /tmp): /tmp/benchxt.sh TAG 100 (bench + xctrace Time Profiler 12 s at 100 s), /tmp/xtparse.py (self),
    /tmp/xtincl.py (inclusive), /tmp/xtcallers.py, /tmp/xtimp.py (cost per import); /tmp/gfps.py, /tmp/gsmooth.py.
- Craft Anything's auto mode blocks running the game under lldb (and `rm`, long inline edit scripts) as
  "delete_outside"; the person can allow it in the project's approvals.
  * 2026-11-18 evening HEADLIGHTS AS LIGHTS (stage 1; street lamps next):
    - The game has no dynamic lights: world shaders know only the sun (cvLocalLightVec / cvWorldLightVector), car
      paint SH (cmLocalColour/Direction); lamps are glow sprites.
    - Data (RECOMP_D3D9_CONST_PROBE=<present> + _NAMES=a,b: per draw values of named VS/PS constants; VS CTAB names):
      vertex constants cmWorldMat (row vectors, translation in row 3) and cmWorldViewProj; a draw with identity
      cmWorldMat gives ViewProj. Car-paint draws (PS has cvVinylScales) give each car's world matrix: x forward, y
      side, z up. The fog pass 961f166f1ae442c4 reads the scene depth packed in texture 6 (stage 2, decoded
      dot(sample, c7) = z/w as D3D) and the sharp scene (stage 1), blurred quarter copy (stage 0).
    - kit d3d9.cpp RECOMP_D3D9_CAR_LIGHTS="ps=..,car=..,first=14[,gain,front,side,height]": per frame collects the
      cars (one per 2.5 m - body parts carry own matrices) and ViewProj; for the fog pass writes c14-17 inverse VP,
      c18 eye (inv row 2 / w) + count, c19-30 up to 6 lamps (2 per nearest car within 150 m: pos + 28 strength,
      dir + cos 0.86), c31 colour; pconst_count 32. Setting core.nfsu/headlights (live).
    - Patch 961f...: world pos from depth and inverse VP, normal from derivatives, spot cone (soft 0.12), 1/d^2,
      weaker under the bumper (smoothstep 1.5-10 m), fade by 70 m; added as scene x (1 + min(L / (8 x local blurred
      luminance + 0.04), 2.5)) - strong at night/in tunnels, a few % in daylight. No shadows (lights pass walls).
    - Checked by dumps with gain 10-25: the light pool lies on the road ahead of the cars (reconstruction right);
      at gain 1 in the sunset spot nothing visible, as intended. NOT yet seen at night or in a tunnel.
    - Stage 1b (person: shadows; lamps only after a while in the dark; tunnel lamps + darker saturated tunnels; far
      cars lit too): cars followed frame to frame (nearest within 6 m), per car the luminance of its PS cmLocalColour
      (car-paint draws; 1.25 = the sun's at the sunset spot) - lamps on after 1.5 s below 0.45, off after 2 s above
      0.7, half-second fades; up to 32 lamps (16 cars) within 600 m; registers c14-17 invVP, c18 eye+count, c19-22
      VP, c23 (shadowed lamps, tunnel), c24.. lamps, c88 colour (pconst_count 89 - the host sends any count). The 4
      nearest lamps march 7 steps (quadratic spacing) toward the lamp through the depth buffer (blocked when the
      surface there is 0.3-4 m nearer than the step), only where their light is > 0.004. Tunnel = the camera car's
      lamp level x day (world cvWorldSunColour luminance 0.5-0.9): darker x0.62, saturation 1.3, bright parts kept
      (settings headlights, tunnel_darkness). GPU at 3024x1964 with lamps forced on: ~7-8 ms vs 6.3.
    - Checked: daylight - lamps off (packaged app log); forced lamps (always=1, gain 40): a van ahead lit from behind
      and the road beyond it in its shadow. NOT verified: whether cmLocalColour drops in tunnels (if it does not,
      tunnels will not switch the lamps on; night should, via the sun); no night/tunnel test spot. Tunnel lamps'
      own light: not done - their positions are not known yet (candidates: the glow sprites' draws).
  * 2026-11-18 late (pics/14: menu veil, pixelated HUD digits/text, tunnel bright with no lamps on):
    - Tunnels: cmLocalColour does NOT drop in tunnels. New sky probe (kit d3d9_metal.mm RECOMP_D3D9_SKY_PROBE=
      "ps=961f166f1ae442c4,depth=2,scene=0", launch.c): every 6th frame the fog pass's packed depth (decoded R +
      G/255 + B/255^2, def c7 of that shader) and blurred scene are copied to shared memory; share of sky (depth >=
      0.9999) and its brightness -> host_d9_sky() (weak default in d3d9.cpp for tests). Day: share 0.25-0.4,
      brightness 0.58-0.85. Car lights: no sky (< 1%) = tunnel (cars within 200 m dark), sky brightness < 0.18 =
      night; a blank frame (all sky, black) ignored; new cars start with lamps off. Tunnel darkening when no sky and
      not night. Tunnel lamps: 16 screen taps in the fog pass, bright (> 0.75) spots above the point within 6 m in 3D
      light it. Not verified in a real tunnel (none on the scripted route) - only the forced state.
    - HUD at full resolution with render scale < 1: the back buffer (first target) keeps the guest size, single-
      sampled (t.full_back); the scene targets stay scaled; draws enlarging a smaller texture get linear filtering;
      the final pass has no depth (sizes differ), HUD draws fine. RECOMP_D3D9_SMALL_BACK=1 = old. MetalFX is then not
      used (frame already full size). Captured: crisp HUD and menu text.
    - Menu veil = the composite's bloom: behind menus the game drops the bloom power cvVisualTreatmentParams3.x to
      ~1.4 (4.3 driving). Composite patches keep the power >= 3 and cut the glow to 30% when it is < 3. The bloom
      setting now scales only .x (PS_CONST_SCALE "name.x:factor" component mask): scaling x,y,z made the menu
      background a grey silhouette at "Off". Captured: clean pause menu at the default.
  * 2026-11-18 night (pics/15: menu highlight bars ran across the screen; tunnel: cars inside did not light up unless
    I was in, lamps did not light, darkening over the whole screen and bluish, headlights dim/uneven/jumping):
    - Menu bars: the FE clips them by STENCIL; the full-size back buffer pass had no depth/stencil (sizes differ).
      Now it gets a fitted depth-stencil of its own size (depth_for_size), cleared like the game's. Checked: save
      menu identical to RECOMP_D3D9_SMALL_BACK=1.
    - Probe grid (d3d9_metal.mm host_d9_probe_grid): 96x64 cells of the probed frame - depth at the centre, the
      brightest pixel's depth/brightness/colour (scene = stage 1, the sharp scene). Car lights: per car a roof test -
      the point 5 m above the car projected into the grid lands on a surface within 10 m across and 2-15 m above it =
      roof; sky = open; anything else unchanged. Lamps on after 1.5 s under a roof / at night, per car. Body matrix =
      the most upright part (wheels' matrices spin: lamps jumped); lamp frames smoothed 0.6/0.4 per frame. Strength
      45, cone cos 0.82, soft edge 0.16, shadow bias 0.6 m.
    - Tunnel lamps: while your car's lamps are on under a roof (or night), grid cells brighter than 0.8 and than all
      8 neighbours are put back into the world (merged within 1.5 m, kept 12 s, within 80 m) and passed as omni
      lights after the headlights (dir.w = -1, colour in dir.xyz, 25 m range).
    - Shader: darkening only where a roof is over the pixel (points 1.5-10 m above hide behind geometry), x0.6,
      saturation 1.12, blue clamped to max(r,g) x 1.02; light accumulated per colour. Checked: forced tunnel state at
      the open race start darkens nothing. Real tunnel still NOT seen by me (none on the scripted route).
    - Not possible yet: the 240SX's pop-up headlights rising - that is the game's own car state.
  * 2026-11-18 later (pics/16, the person's I-20 west tunnel): stepped/noisy darkening; headlight direction followed the
    steered wheels; lamps not lighting; other cars dark in the tunnel; asks for the 240SX pop-ups.
    - Body = the car-paint part with the most triangles (d.primitive_count); "most upright" picked a front wheel.
    - Darkening: roof amount = max over 6 points 1.2-11 m above of a soft gap (0.3 + 1.5 m), times not-sunlit
      (1 - smoothstep(0.38, 0.72, luminance)); grade x0.6 warmed (1.05, 1, 0.88).
    - Lamps: grid cells >= 0.42 and >= 1.45 x their 8 neighbours' mean, above road height (camera car z + 1.2) and
      not within 3.5 m of a car (tail lights, the headlights' own pools); strength 14, range 20 m.
    - Car roof test: the surface hit within 40 m across and 2-15 m above (the view to a point over a far car meets
      the ceiling well before it).
    - Pop-ups: the game's NIS_240_SX_89 GEOMETRY.BIN has only closed lamps (KIT00_HEADLIGHT_LEFT_A/B/C), no raised
      geometry: the game itself never raises them; not done.
    - The I-20 west race (person's profile, save "ru": PKGSRC="NFS Undercover" /tmp/pkgrun.sh, keys 32 Return, 44
      Down, 45 Return, 60 Return, 75 Return) starts in the open; the tunnel is further on - not reached by script.
  * 2026-11-18 latest (the person: tunnel darkening still bad, headlights came on even more seldom; "remove that
    darkening, make the game's own tunnel shade darker"):
    - The darkening filter is OUT of 961f166f1ae442c4.patch; the kit passes c23.y = 0; setting tunnel_darkness gone.
    - The game's own tunnel darkness is BAKED: every world vertex shader gives its pixel shader
      2 x max(const, v1.x) (v1 = baked vertex light; 1 = lit) - roads in ot[2].w, buildings/walls in ot[0].z, a few
      in r[0].w - and the pixel shader multiplies the lit colour by it. (Constants seen: cvWorldAmbientColour
      0.205/0.148/0.117, cvVertexPowerBrightness (1.3,1,1,0.3) in the road PS c8 only as a max brightness factor.)
    - New kit feature: <patch dir>/all-vertex.patch (and all-pixel.patch) - @@REGEX/@@REPLACE/@@END blocks applied
      to every translated shader, @ENV(NAME,default) = RECOMP_<NAME>. mods/core/nfsu/shaders/all-vertex.patch takes
      that baked light below the pivot 0.85 to a power: core.nfsu shade_depth (restart) 0/1/2 -> launch.c
      RECOMP_NFSU_SHADE_POWER 1.0/1.8/2.6. 151 shaders matched in a race. Open daylight frames: mean brightness
      unchanged (103.9 vs 103.2). NOT seen in a tunnel by me.
    - Note: only .patch files (and .toml) are copied into the app by kit/cmake/MacBundle.cmake - hence the name.
    - Headlights: roof test now 7 points over the car (5 m and 9 m up, +-10 m along, +-5 m across, 7 m up 5 m
      ahead): sky at any = open, a surface 2-15 m above at >= 3 and no sky = roof, else unchanged (one point took
      tree crowns for roofs: a traffic SUV lit in daylight). Dark/bright timers run back down at half speed instead
      of resetting on an undecided frame; on after 1.0 s dark. A 40 s daylight drive: no lamps on.
    - RECOMP_D3D9_CONST_PROBE now also logs each pixel shader's constant names with registers (name@cN).
  * 2026-11-19 (pics/17, a parking garage at the Palm Harbor rally start: too bright, no lamps, no headlights - "nothing
    lights anywhere"): the person's log shows all-vertex.patch applied (66 shaders); it had no car-lights logging.
    - The game lights tunnels/garages with its SUN (ceilings cast no shadow): with the roof forced, dimming
      cvWorldAmbientColour alone moved shaded grass 41.8 -> 39.9; with cvWorldSunColour too 41.8 -> 20.5, road 155 -> 136.
    - Kit: g_d9_roof (0..1) = the camera car under a roof (0.7 s to decide, 1 s fade, 0 at night/headlights off);
      PS_CONST_SCALE rules "name:factor@roof" follow it per draw; RECOMP_D3D9_ROOF_FORCE=<0..1> for tests.
      launch.c by shade_depth: 1 -> ambient 0.35, sun 0.3, cmLocalColour 0.5 @roof; 2 -> 0.2/0.15/0.35. The world
      seen through an exit dims too; the sky does not.
    - Tunnel lamps are looked for as soon as the roof level > 0.3 (before: only after your own headlights came on -
      in the garage they never did, so nothing lit). Roof test counts surfaces from 1.4 m above the car (low garage
      ceilings were under the old 2 m).
    - Always-on log line on each change: "car lights: your car under a roof / in the open / undecided (sky, cars,
      lamps)" - the person's next log says what the roof test saw. NOT seen in a garage or tunnel by me: the
      dev profile copy of the person's profile plays the intro instead of loading "ru" with the usual keys.
    - /tmp/dev.sh takes PSCS=<PS_CONST_SCALE rules>.
  * 2026-11-19 later: the person saw no difference and asked to REMOVE the extra lights and the tunnel shading, keeping
    the game's sun, its saturation/brightness and HDR. launch.c no longer sets RECOMP_D3D9_CAR_LIGHTS,
    RECOMP_D3D9_SKY_PROBE, the "@roof" rules or RECOMP_NFSU_SHADE_*; PS_CONST_SCALE is back to gloss + bloom.
    all-vertex.patch and the 961f lighting block moved to docs/nfsu-lights-experiment/ (not built); 961f keeps only
    the blur-blend removal. Settings headlights and shade_depth removed (19 settings). The kit features stay (off
    unless set). Packaged run: 961f "1 applied", no car-lights lines, HDR scene live.
  * 2026-11-19 GAME MODS (dist/mods_to_add: the person's 11 mods; "integrate natively, switch live in F10 Mods; cars
    and police mode off by default, the rest on"). Phase 1 done:
    - TexMod packs, kit host/texmod.cpp (RECOMP_D3D9_TEXMOD="setting=path|..."): .tpf = ZIP with every dword XOR
      0x3FA43FA4 (tail bytes ^0xA4), entries ZipCrypto with TexMod's fixed 42-byte password, texmod.def lists
      "0x<hash>|<entry>". The hash is CRC32 (zlib table, init ~0) of bits*w*h/8 top-level bytes WITHOUT the final
      inversion (the standard CRC matched nothing). d3d9.cpp texmod:: hashes a 2D texture's level 0 when uploaded,
      binds a replacement host texture (ids from 0x7f000000, made from the DDS: DXT1/3/5, 32/24-bit) while the pack's
      setting is on (re-read every 30 presents). Redux NFSUCRedux.tpf: 76 textures, 40 matched at the I-20 start;
      ProStreet sky: japan3 4 + "Sky shit" 5.
    - File overlay, kit kernel32 RECOMP_FILE_OVERLAY="setting=<game path tail>=<host file>|...": read-only opens
      whose name ends so read the mod file while the setting is on (next open). Redux GLOBAL\InGameA.bun and the
      high-contrast HUD's GLOBAL\HUDTEXRACE.BIN (both the same size as the originals); log "read from the mod file".
    - Photorealistic (its ReShade preset LiftGammaGain/qUINT Lightroom/Smart_Sharp/PD80 bloom, reimplemented, not
      copied): a block after the vignette line in both composite patches (823d/7f36), on when c30.x > 0.5; kit
      RECOMP_D3D9_PS_SETTING_REG="<ps code>:<reg>:<setting>,..." puts the setting in that register. Scene only.
    - Settings (group visual, live, default on): mod_redux, mod_prostreet_sky, mod_hc_hud, mod_photoreal. package.py
      --mods (default dist/mods_to_add) copies the five files to Resources/ModData (MOD_FILES); launch.c sets the env.
      Kit TOML help strings don't unescape "\\": use "/" in help text.
    - Checked: dev run with all on (sky overcast from ProStreet/InGameA, Redux roads, dark minimap, 360 race icon,
      warmer fuller colours); packaged app: packs load, 40 textures replaced, both overlays read; F10 Mods tab dump
      shows the 4 rows in Russian.
    - NOT done yet: VltEd scripts (Redux UCPO.nfsms 683 update_field - camera, postfx, sunandsky, shader params...;
      Revamped Pursuits 5541 update_field + resize/add/delete_field) need an attribute-vault layer; GenericFix
      (ShadowsRes 3840, CSM 100/175/300 vs 5/30/170, SimRate = refresh, Bloom 0) and ExtraOptions (hotkeys,
      collector's edition, no double-tap reverse) need their exe sites; the 5 addon cars (CARS/<name>, Binary
      DB edits, .nfsms VltEd) and Police Career (an ASI built for the SecuROM 46.6 MB exe 6caed72e...; its own
      gameplay.bin variants) are not integrated.
  * 2026-11-19 pics/18 (pallets vanish close by, short shadow/scenery distance, grainy road and ground, rainbow-pink
    windows, a wide light band down the middle near the sun; "do the rest, HDR off for now"):
    - HDR off: hdr_output default false (table, defaults json, launch.c fallback 0) and in the person's profile.
    - GRAIN: 54 of Redux's 76 DDS have no mip levels. texmod.cpp builds the chain in a background thread (DXT1/3/5
      decode, 2x2 box, bounding-box re-encode; add_mips), the shim draws the game's texture until it is ready
      (host_texmod_state includes a finished-count so bound() re-decides). Log "smaller levels made".
    - BAND: "Sky shit.tpf" (the ProStreet sky mod's second pack) is lens flares, not sky; its F3640B31 is a 512x64
      streak with a flat band across the whole width that the game draws vertically near the sun. Left out
      (texmod "<path>!<hash>,..."). NOT seen on screen by me (the spot is the person's I-20 west sun view).
    - PINK WINDOWS: Redux's InGameA.bun changes only sky texture data (SKY_STYLE layers) + a mesh's indices, no window
      texture (WINDOWREFLECTION untouched); none of the TPF images is a window. Most likely the HDR lift on the window
      reflections (HDR now off). NOT reproduced.
    - DRAW DISTANCE (setting draw_distance 0/1/2, default 2, live; GenericFix's sites found by its byte patterns):
      game.toml operand_redirects move FUN_00780dc0's cascade-distance stores (00780deb/00780e29/00780e31 ->
      0x00d53f38/3c/40) to slots 0x01375800/04/08 and instruction_patches 0082b0e6 reads FUN_0082b070's per-view
      scenery layer mask from slot 0x0137580c (seeded 0x0e); core.nfsu draw_distance_apply (main-loop pass) writes
      the array = max(game's value, 15/70/320 or 25/120/500 m) and the mask 0x0e / 0x0f (bit 0 = GenericFix
      ImproveSceneryLOD). launch.c adds "3072x1024:2" to TARGET_SCALE when draw_distance > 0. The game's lowest
      world detail used mask 0x0c - with the slot it is 0x0e at "as in the game" (acceptable). GenericFix's
      DisablePreculler ([0x00d3ce8c] = 0) and BruteforceCulling (NOP the JE at 0082b03f) not used.
      Checked: I-20 east start, level 0 vs 2 - more trees/props in the distance at 2. The pallet spot not reached.
    - A help string with \" made the kit reject the whole mod (game never presents a frame): use ' in help text.
  * 2026-11-19 pics/19 (a mirror-like wet strip at the road's edge; the VltEd parameter mods):
    - VltEd scripts live: mods/core/nfsu/attrib.inc vlt_pass (each main-loop pass, works every 2 s). launch.c
      RECOMP_NFSU_VLT="mod_redux=ModData/redux/UCPO.nfsms|mod_revamped_pursuits=ModData/pursuits/RevampedPursuits.nfsms"
      (package.py MOD_FILES; Revamped Pursuits = its "Vanilla/Cop Perf" variant: Collection edits only, racers' cars
      untouched). Only update_field/resize_field; a script with "ui_option installation_type" markers -> the
      "install" block only. Writes remember the original bytes and are put back when the setting goes off
      (mod_revamped_pursuits new, default on; mod_redux now also covers UCPO). Arrays too small are copied into
      guest_alloc memory and the node's pointer moved (pointer nodes only). Numbers parsed without strtod (locale).
      Checked on the PQ profile (log, NFSU_VLT_LOG=1): Redux 665 of 683 set (8 camerainfo STIFFNESS[n] on arrays of
      count 0, 10 "envmap_sky data N" render-state lines left out), Revamped Pursuits 1459 of 1462 + 10 arrays grown
      (3 NISCars PresetRideCollectionKey members unknown). camerainfo/close/FOV read back [78,78] (was [71,78]).
      UCPO ends with BloomFactor X 0 / Y 0 (its last section) - that is the script, not a bug. The tests could not
      reach driving (screen locked: window "focus no", the game inactive), so nothing was seen in a race.
      NFSU_ATTRIB_DUMP="class/coll/field,..." (+ NFSU_ATTRIB_DUMP_AT=s) reads values back.
    - Wet strip: the road shaders' own environment reflection. launch.c PS_CONST_SCALE adds
      "cvReflectionParams.y:0.3" (follows gloss). ShadersLibrary.bin's CTABs: exactly 20 pixel shaders carry
      cvReflectionParams, all road ones (ROADBASE/BLEND_SPEC_REFL samplers) - no car shader. Not seen on a frame yet.
  * 2026-11-19 ExtraOptions built in; Police Career assessed (the person: "do everything, adapt what is incompatible"):
    - ExtraOptions (Photorealistic/scripts/NFSUCExtraOptions.asi) is made for THIS exe ("v1.0.0.1 STEAM nfs.exe
      10,584,064 bytes"). Its patches were found by running its DllMain under Unicorn beside our exe's image and
      recording every byte written into the image (tracer kept in /tmp/asitrace, not in the repo), once per ini key.
      Built in (mods/core/nfsu/extraoptions.inc, settings mod_eo_*): Collector's Edition = byte [0x00daa166] held
      at 1 (its own patch: setne dl -> mov dl,1 at 0x0052555e); no double-tap reverse = the 0.5 s window read at
      0x00428dc5 from 0x00bd90f8 redirected to slot 0x01375810 (game.toml operand_redirects + data_seed; 0.0 when
      on); hotkeys from its routine (sub_10003620): F5 unlock-all ([0x00daa15a], setting mod_eo_unlock_all), F6 auto
      drive (FUN_00663220/00663290, arg 1), F7 heat hack (min/max [0x00d1f0c8]/[cc]), PgUp heat via
      [[0x00de99dc]]->v1->[+4]->FUN_005f6700(0x00402240)->v2(float), Scroll Lock cops FUN_0065c7c0(flag), O
      emergency lights FUN_006eafd0/006cc110/006cc120(car, 0xf000) with the car found by matching v1() of
      [0x00de93e0+4i] against [0x00de99e8], Pause [0x00d89950]; messages FUN_00661bb0("^text", 0). Driving check:
      [0x00da57b8]==6, [0x00deb99a]==0, FUN_00521960()==0. NOT built in: headlights (it rewrites code at
      0x006d1f74/0x006d2021 at run time), debug camera, its copyright-text takeover (its branding), the other
      always-installed code hooks (they read its values, which equal the game's at the person's ini).
      Checked: the packaged app loads core.nfsu and presents; the redirected FLD is in chunk_004.c. Keys not tried
      in a drive (the screen was locked: window without focus).
    - POLICE CAREER: its required exe is the retail 46.6 MB 1.0.0.1 (6caed72e...), NOT ours (a separately linked
      build: functions shifted by varying amounts, .rdata/.data +0x1000). Its .text is plain code, but SecuROM moved
      parts of game functions into .securom (219+ jmps such as 0x0044adf4 -> 0x01dcfdc6), so the port cannot move to
      that exe. The ASI (mingw, 460 KB code, C++) checks that exe's SHA-256, verifies byte signatures at its
      addresses, references ~2-4 thousand addresses of that build (code and data) and patches code at run time
      (4-byte cmpxchg of call/jump fields, byte blocks; VirtualProtect at sub_6c9c20a0/2730/2ee0). Adapting it =
      an address map retail -> ours for every reference, translating the ASI as a kit aux module, and turning its
      code patches into kit hooks. Not started beyond this assessment.
  * 2026-11-20 POLICE CAREER RUNS (adapted plugin; tools/police_career.py, tools/dev/asi/*, kit aux module
    [modules.aux.c_policecareer] at 0x10130000, loaded by core.nfsu's LoadLibraryA when mod_police_career is on):
    - Boot to driving with the plugin: every subsystem initialises, the Interceptor gameplay (SCRIPTS\
      PoliceCareerGameplay_steam.bin) mounts, the save loads with police defaults, first player marker update, 200 s
      without a fault.
    - The last crash ("Out of memory, allocating 4294967220 bytes ... 'GamePlay'", then a call to 00a396e2 = the
      allocator's fatal path) was MY adapt.py: the 4 bytes after ".BIN" ("BIN\0" = 0x004e4942) look like a code
      address and were remapped to ".2FN", so the file was not found and its size -1 went into the loader. adapt.py
      in_text now keeps short NUL-terminated strings (3 printable bytes + NUL after a printable byte, or unaligned):
      7 fields (".BIN", "auto", "false", "this", "float", "long", "half"). LOG=2 shows each CreateFileA.
    - Translator code_patch_sites also take MOV r32,[abs32] (8B /r mod 00 rm 101) and MOV EAX,[abs32] (A1): the
      address is read from memory. 0x0042fa31 `mov ecx,[0x00de99dc]` is repointed by the plugin at its own
      variable. patchcheck.py PC_LIST=1 lists every pointer patch: only 0x663575, 0x663605, 0x42fa33 are in code.
    - kernel32 FlushInstructionCache no longer warns for patches in data sections (vtable slots, pointer cells are
      read from memory anyway) or for the forms code_patch_sites handles (logged at LOG=2 only).
    - NOT verified: police mode itself (free-roam pause menu "police cars" entry) - tests run on a locked screen.
  * 2026-11-20 ADD-ON CARS (setting mod_addon_cars, default off, restart; tools/addon_cars.py at packaging time,
    mods/core/nfsu/addoncars.inc at run time). The game reads its vaults from GlobalMemoryFile.bin and GLOBALB from
    GlobalB.lzc, so VltEd/Ed's file edits are redone as additions:
    - Vault "addon_cars" (Game/GLOBAL/ADDONCARS.BIN): every collection the cars' .nfsms create. copy_node = the
      source collection's header, types, entries and layout block with every data block and string they point at
      (block = from a pointer target to the next one), new key/parent, CollectionName (0x9ca1c8f9) renamed;
      add_node = the parent's layout, no entries; update_field/resize_field/delete_field applied on the copies
      (members: Vector X/Y/Z/W, RefSpec Class/Collection, graph 0xb2d362a1 MinX/MaxX/MinY/MaxY at 0/4/8/12,
      0x9df6ff3b Front_/Rear_ graphs at 0/48, 0x4cb36381 Front/Rear; StringKey a502a824 = {jhash, string}).
      VPAK format as PoliceCareerCars.bin: bin = StrE + blocks, vlt = Vers StrN DepN DatN (exports, 8-aligned)
      ExpN PtrN (section 1 = bin fixups, section 0 = vlt fixups, all destinations bin offsets).
      Mounted AFTER FUN_006ac7d0 (LoadCarsVault; its stage 005f77d0 is a vtable slot of a boot stage at 0xbfb870
      and NOT a listed function, so the kit refuses hooks on it) with FUN_006abbc0 / FUN_006ac6e0 / FUN_0069e670.
    - GLOBALB (ModData/addoncars/GLOBALB.BUN, read in place of GLOBAL\GLOBALB.LZC through the file overlay - the
      bundle loader takes the unpacked form under that name; GlobalB.lzc = 'RFPK' + one RefPack stream):
      CarTypeInfo 0x34600 (rows 0x130: name 0x00, base 0x20, geometry path 0x30, 0x70 name, binhash 0xb0/0xf0,
      UsageType 0xf4, binhash(CarMemType) 0xf8, Skinnable 0x127, binhash(paint) 0x12c; count = size/0x130,
      FUN_00858650; lookup FUN_00843630 by 0xb0); collisions 0x8003b900 (one 0x3b901 per car, key jhash(name) at
      chunk+0x40); parts 0x80034602 (34603 header counts, 34606 strings, 3460c u16 attribute lists, 34605
      (key,value) attributes, 3460b sorted model hashes, 34604 rows {u8 0, u8 model, u16 list, 12 runtime bytes}
      sorted by model - FUN_00538f10 binary-searches them; part names are <model name>_<kit>_<part>_<lod>, so a new
      model's rows are copies of the source car's; SPECIFICCARNAME 0x8e73b5dc parts of other models get copies).
      Every grown chunk is followed by a padding chunk so later chunks keep their offset mod 0x80.
    - Car lists: appended at run time (the scripts' slots 75-133 of lists grown to 118/500 with empty rows; the
      lists hold 52/59/60), only after the mount. Names (Ed's RESOURCEn labels, CARNAME_<MAKER>_<MODEL> uppercased
      binhash) answered by a REPLACE hook on FUN_00571b80 (the text lookup).
    - Checked (packaged test app, with Police Career on too): vault mounted, 92 car types, 11 list entries, the new
      collections read back from the game's own tables (Regera MODEL, Cost 1000000, TORQUE MaxY 1300, references
      resolved). NOT seen: a new car bought, loaded or driven (tests run on a locked screen; no add-on model file was
      opened in a 140 s run). Left out: stockPaint member lines, resize_collection, CCXR's FRONT_WEIGHT_BIAS (not in
      the copy), the Miura's Binary collision/parts (its source car's are used instead).
  * 2026-11-20 evening (the person: police mode buttons dead, then an unknown blue Shelby; a crash on the first
    collision; add-on cars not in the shop; dist/pics/20 = the plugin's empty "Police cars" menu):
    - CRASH = a kit bug in call-site redirects: the plugin's replacement for the call at 0x00433209 (to 0x005f6700),
      fn 0x10160440, ends with `jmp [0x101d2ca0]` (the original). A JMP keeps the caller's return address on the
      stack, so mods_hook_dispatch saw [esp] = 0x0043320e again and redirected to the plugin again: 34832 nested
      host frames, SIGSEGV in the stack guard (crash reports 09:03/09:05, recursionInfoArray). Fix: generated
      recomp_jump sets __thread recomp_dispatch_via_jump around a hooked dispatch; mods_hook_dispatch skips
      redirects for it (kit/mods/hooks.cpp, translate.py). The same loop ran (once more, or until a guard) for every
      plugin replacement that tail-jumps to its original - the likely cause of the dead police-menu buttons too.
    - Police presets: FUN_00524d60 (thiscall: car-record list [0x00d8e1a4]+0x88, preset) + FUN_0051d410 fill the
      FIRST row whose [0] equals the preset key; a preset that already has a row leaves the new row empty (game
      behaviour). Called directly on the person's save (NFSU_PRESET_TEST=<preset> [NFSU_PRESET_TEST_PC=<vehicle>
      calls the plugin's own FUN_10170840], NFSU_PRESET_TEST_AT=<s>): works, row 104 found. Not re-tested in the menu.
    - Shop: the dealer list (FUN_00528e40 from the boot stage at 0x005f78b0, 0x14a rows of 7 dwords {slot, frontend,
      pvehicle, ...}) already held the 5 add-on cars (logged by an AFTER hook); they kept their source car's tier
      (frontend UnlockedAt 6 = CCX's, the last tier), so the lot hid them. tools/addon_cars.py now sets UnlockedAt 0
      on the new cars' frontend records (manufacturer nodes keep 15).
    - Person's save note: PoliceCareer.log "Cannot repair career active car FFFFFFFF" on their "ru".
  * 2026-11-20 night (the person, dist/pics/21 video: cops ignore them, a blue Shelby with no lights, no way out of
    police mode or to start a chase, the police car menu opens nothing, still no new cars in the shop; wants delete-save):
    - ROOT CAUSE of the dead police menus and the Shelby: the career/garage object [0x00d8e1a4] keeps its car-record
      list at +0x88 in our exe but +8 in retail (every field >= 8 is 0x80 further on). The plugin reaches it only
      through the retail global 0x00da1f24 (+8 in FUN_101711f0/FUN_10171230, `lea 8(%edi)` at 0x1017144a). Fix:
      tools/police_career.py SLOT_OVERRIDES 0x00da1f24 -> cell 0x01375870, which core.nfsu (police_career_pass in
      nfsu.c, each HUD pass) keeps at [0x00d8e1a4]+0x80. Checked on a copy of "ru": the plugin restored the active car
      to row 0xD8 (their CCX). Menus not seen on screen.
    - Shop: loading a save REPLACES the dealer list (it is saved), so the boot-built add-on rows vanish.
      addoncars.inc addon_dealer_sync (after the profile loads, every 2 s): for each "dealer <pv> <fe>" line in
      addoncars.txt with no row yet, the first free row ([0] == -1) gets {index, fe, pv, 0} and FUN_0051d7c0
      (fastcall row) fills its flags. Checked on "ru": rows 217-221 added, flags 0x20001 like the CCX row.
    - Chases: the plugin's ToggleEmergencyLights ("Мигалки") is on H (DIK 0x23); leaving police mode is the same
      pause item.
    - Saves: the game's save screen (APT) has no delete. The F10 menu has a 4th page "Saves" (kit settings_menu.cpp,
      only when RECOMP_SAVES_DIR is set; launch.c sets <profile>/Documents/NFS Undercover). Delete takes two presses
      on the same line and moves the folder to the Trash (host_trash_path in menu_glass_mac.mm via NSFileManager;
      weak default -2 -> rename into $HOME/.Trash, used by mods_tests suite menu_saves_page).
  * 2026-11-20 midday (dist/pics/22: MISSING STRING names, Miura without a model, police camera close, arrest text
    shown several times, police title off the edge, no way out of police mode; then F10 pause, F10 redesign,
    Extra Options keys for a MacBook, the Porsche Pack):
    - Names: FUN_00525070(frontend key) builds "CARNAME_<maker>_<model>" (the right labels, logged by
      NFSU_ADDON_NAMECHECK=1) and asks FUN_00571b80 - which Police Career slot-hooks; the kit ran the function's body
      directly when the plugin continued it, skipping every other hook (core.nfsu's addon_text). Kit fix
      (hooks.cpp): the continuation runs the hook chain. Belt and braces: core.nfsu answers the label for an add-on
      frontend key itself (REPLACE on 0x00525070; "dealer <pv> <fe> <label>").
    - Police mode exit: the plugin ends the role only when the pause menu's own cars screen (FE id 0x11) opens.
      core.nfsu (police_role_log): role on + the career's active car (FUN_00820c70("UserData","UDKey","UDValue",
      0x307d9226), as the plugin reads it) is not a police model and not the plugin's police car, twice a second
      apart -> role byte off, racer slot = that car. Role state logged on change ("core.nfsu: police role: ...").
      Plugin profile block [0x101d69a0], role object +0x20: role +0x18, police car +0x1c, racer car +0x20.
    - Title: tools/police_career.py TEXT_PATCHES "Полицейские машины" -> "Полиция" (UTF-16 in the plugin).
    - F10 pause never happened: the pause point is in Present, NFSU presents from its render thread and the menu
      allowed only the main thread. RECOMP_MENU_PAUSE_ON_PRESENT=1 (launch.c) lets the presenting thread park.
    - F10 redesign (kit settings_menu_draw.cpp): pixel scrolling clipped to the list, natural direction (SDL FLIPPED
      wheel negated for the menu only), strong selected row, value column with fixed arrows, sliders for numbers with
      >= 8 steps (drag), "restart" badge + status line for restart-only settings changed this run, Apply/Discard
      always in the footer (feedback when nothing waits).
    - Extra Options: own Mods section (group "extraoptions"), switches for autopilot, heat level (1-5), police,
      emergency lights, frozen camera; keys 6 7 8 9 0 O - flip the switches.
    - Porsche Pack (26 cars, one main.nfsms with "/// name ///" blocks, Ed Configs/<NAME>.ini): addon_cars.py packs,
      "in <group>" lines (mod_porsche_pack / mod_addon_cars), GLOBALB_ALL.BUN with the pack's types read only while
      its switch is on. BOOT HANG found on the way: write_vault padded 8 bytes between DatN and ExpN, the game's
      chunk walker (FUN_00466c70) read a size-0 chunk and looped for ever; DatN is now padded to 16 inside the chunk.
    - mod.toml: the kit's TOML reader takes no escapes - an escaped quote in a help text rejected the whole core mod
      (gen_nfsu_settings.py now asserts).
    - Car lot names (2026-11-20 later): the car lot reads its text past every hook, so the names are now real game
      strings: addon_cars.py writes every language's LANGUAGES/<lang>_Global.bin with the labels added (first 0x39000
      chunk: count, entries offset, strings offset, 16-byte name, (hash, offset) rows sorted, texts; following chunks
      kept 16-aligned with a type-0 padding chunk) into ModData/addoncars/languages; kit RECOMP_FILE_OVERLAY takes a
      folder rule ("<setting>=LANGUAGES\=<dir>", lower-case file names). Seen in the Europe car lot: Koenigsegg Regera,
      Lamborghini Aventador / Miura P400, Porsche 911 GT2 RS (991), 918 Spyder, 935/77.
    - F10 pause done properly: RECOMP_MENU_PAUSE_AT=6ae4c0 (main-loop step) parks the MAIN thread (kit hooks.cpp);
      parking the render thread at Present froze the menu (no host pump, no keys). Seen: paused 12 s, keys moved the
      menu, resumed.
    - Police camera: class "camera", collections close/far, field 0x9f743a0e = distance graph {?, speed, d, d, speed,
      d, d}; scaled while police mode is on (NFSU_POLICE_CAM percent, default 115), restored after. At 125% the car
      went from ~51% to ~36% of the screen width at the same spot.
    - "Гонщик арестован" repeats: HUD message vtable 0x00bf8cb4 slot 1 (0x005cce50); the plugin's args are type 2,
      priority 3 ([+0x224] is a priority, not a count) and it logs one successful submit per arrest - cause not found.
    - Miura without a model (checked in the car lot with frame dumps, 5 s on the row): its GEOMETRY.BIN is read
      (absolute-path opens, like REGERA's) but nothing is drawn. Not the parts (Murcielago's rows tried), not the
      base car (whole car swapped to lam_mur_lp_06 tried), not the damage file (now also copied as LAM_MSV_71.damage).
      Difference found: every add-on that draws (SVJ, Regera...) packs its solids in a 0x55441122 chunk after the
      solid list header; the Miura's GEOMETRY.BIN (NFS-CarToolkit Apr 2019) has 108 plain 0x80134010 solids. Next:
      find the game's loader path for unpacked solids (or a port bug in it).
      Later the same day (the person sent the mod page: install = VltEd scripts + Binary "Import Node" of its
      CarTypeInfos/Collisions/DBModelParts nodes, gMpB/RAWW containers). Tried and NOT the cause: packing the plain
      solids into 0x55441122 RAWW blocks with the 0x134004 solid table updated (entry {key, offset, stored, length,
      flags 0x200 packed}; tools/addon_cars.py pack_geometry, PACK_PLAIN_MODELS off). The mod's DBModelParts is
      Gallardo-based (KIT00/01/06/11/W01, "LAM_GAL_560_09_CV" on body kits); its texture pack hash 0xdf59a0ab
      (TOOLKIT tpk) - next: compare the texture-pack lookup, or import its Binary nodes exactly.
    - MIURA FIXED (2026-11-20 evening): the game finds a car's type through its pvehicle collection's own name
      (lam_mur_lp_06 <-> LAM_MUR_LP_06 for every stock car). The Miura's scripts name its collections lam_mr_sv_71
      while its car type is LAM_MSV_71, so the car lot never loaded the model. Proven by elimination: the SVJ's model
      and textures renamed as the Miura's (tools: /tmp/svj_as_miura.py, JDLZ decoder /tmp/jdlz.py) stayed invisible
      too, so the files were never the cause. tools/addon_cars.py renames such collections after the car type
      (lam_msv_71) and writes "rekey <old pv> <new pv>"; core.nfsu's dealer sync moves saved career rows to the new
      key (the person's save had the Miura row 221). Seen in the car lot with the person's recorded path
      (RECOMP_KEY_RECORD, kit main.cpp: replayable KEY_SCRIPT steps). Diagnostics kept (env-gated): NFSU_HASH_LOG,
      NFSU_HASH_PREFIX/TEXT, NFSU_SOLID_KEYS (FUN_0053f3b0 lookups), NFSU_PRINTF_LOG; addon_cars.py refpack() and
      pack_geometry() (PACK_PLAIN_MODELS off - not needed).
  * 2026-11-20 night (the person's list: money with unlock-all, H ends the police chase, more add-on cars, Hard Drivers):
    - MONEY: the career cash is the game's value 6006 (0x1776) in its value store 0x00d2a178: FUN_00519540 get(id,
      &out,-1,-1,-1), FUN_005195a0 set, FUN_00519600 add (handler per id/1000 at store+8+4*group; group 6 = 0 when
      no career). Found through Extra Options' StartingCash (its cave pushes the cash in place of the 0 that
      FUN_00647e10 sets at a new career); the game's producer demo sets 10 000 000 (FUN_005e76b0). core.nfsu
      extraoptions.inc tops it up to 10 000 000 every 0.5 s while mod_eo_unlock_all is on (NFSU_EO_LOG logs it);
      off leaves the money.
    - POLICE CHASE: the plugin's pursuit controller = [0x101d69a0]+0x13e4, state dword (0 idle, 1 invitation,
      2 dispatch, 3 pursuit; FUN_1013b560 its update). Its own reset FUN_1013ae90 (run by the plugin when the player
      leaves the world, FUN_1016d180, and by an Interceptor cancel, FUN_1013bdb0) releases the native pursuit, the
      police (FUN_10137af0 when state 2/3) and the HUD. H down while the role is on and the state is 2/3 (read at
      key-down, before the plugin takes the key) -> core.nfsu calls it next pass, logs "police pursuit called off by
      H" and shows "Pursuit called off.". The Interceptor job (its own object +0x1514, FUN_1013c890) is not touched.
    - ADD-ON CARS +4: Brera, One-77, AMG GT R (Ed-config mods, picked up as before); the Honda NSX-R GT is a VltEd
      install script ("game UC", "script files\..." lines) + Binary .end: find_cars now reads such a folder - the
      scripts with copy_node/update_field are the vault part, file_copy lines the model files, the .end's
      add_or_update_string CARNAME_* the name and copy_collection DBModelParts/Collisions the part rows (PAG_ZON_F_06)
      and collision (LOT_ELI_111_06) sources (info parts_from/collide). Its logo/vinyl textures are not added (needs a
      TPK writer). 35 cars in the run-time list now.
    - COROLLA: a REPLACEMENT of the Nissan 240SX (its solids are named NIS_240_SX_89_*): setting mod_corolla
      (restart) reads CARS\NIS_240_SX_89\GEOMETRY.BIN/TEXTURES.BIN from ModData/corolla (overlay) and applies its
      wheel script through RECOMP_NFSU_VLT. The name stays the 240SX's.
    - HARD DRIVERS: mod_hard_drivers int 0-3 (Off/Easy/Normal/Hard, live). RECOMP_NFSU_VLT items may be
      "<setting>#<n>=<script>" (on while the setting is n); up to 8 scripts. attrib.inc learned graph members
      (glue CatchupVsSkill etc. 48 bytes {MinX, MaxX, MinY, MaxY floats, GraphScale at 16}; SpreadOverRace pairs
      Easy_/Hard_ at +0/+48) and short inline Text numbers (gameplay CatchUpSkill holds "0.2"; type 0xa3f0c234
      EA::Reflection::Text), written only over bytes that read as text. Gameplay collections are named with paths
      ("race_bin_career/E020"; key = hash of the whole name, checked in GLOBAL/gameplay.bin).
    - BUILD TRAP: tools/build.py does not rebuild core.nfsu when only an .inc changed - touch mods/core/nfsu/nfsu.c.
    - GAMEPLAY VAULTS (checked live with NFSU_VLT_TABLE, 2026-11-20): gameplay.bin holds many vaults - gpcore (3377
      gameplay collections, always loaded; the live class table had 3378) and one per race bin/event (race_bin_career
      199 = race_bin_career/E020 ..., E223, cp_c_02 ...), loaded by the game only with their race. Hard Drivers' 728
      per-race edits therefore land when a career race loads (the pass retries every 2 s); its 97 general values
      (glue, basicrace CatchUpSkill, aivehicle racers ...) apply at once. VltWrite now remembers its collection and
      where it was loaded; switching a script off puts a value back only where that collection still is.
      CatchUpSkill is a POINTER to its text in memory (inline only in the file): the new text goes into a guest copy.
  * 2026-11-20 late night (the person: the Honda must have its logo and vinyls; the Corolla must be its own car):
    - COROLLA IS NOW AN ADD-ON (mod_corolla removed): tools/addon_cars.py CONVERT {'corolla': TOY_COROLLA_1 (same length
      as NIS_240_SX_89), frontend corolla under toyota (manufacturer 9, read from supra), "Toyota Corolla"}.
      rename_car_files: every name NIS_240_SX_89* -> TOY_COROLLA_1* in the model (strings) and every bStringHash of
      such a name in 0x134011+16, 0x134003/0x134004 (re-sorted by key), 0x134012 and 0x134902 (texture refs),
      0x13401d; the texture pack's entries ({0x55441122, dsize, csize, dst, rel, 0} + JDLZ/RFPK; TextureInfo at 0 of
      the block: hash +0xc, name length +0x58, name +0x59, fourcc +0x88; picture after 0x100) unpacked, renamed and
      packed again with the new jdlz() encoder (round-trip checked); hash list 0x33310002 and rows 0x33310003 sorted
      by hash. The game builds car texture names "%s_SKIN1/_MISC/_INTERIOR/_BADGING..." from the car type, hence the
      whole rename. Vault: copies of the 240SX's pvehicle/car/car_parts/ecar plus the mod's ecar edits; copy_node
      accepts a source key "0x........" (the 240SX's frontend name isn't known). Damage = the game's 240SX file.
    - HONDA EXTRAS from its Binary .end: tpk_add_textures (plain packs: 0x33310004 infos of 0x59+name bytes padded to
      4, 0x33310005 formats, pictures 0x80-aligned in 0x33320002; packed packs: one 0x55441122+JDLZ block per new
      texture, big game entries are several 0x8000 pieces with the info last - copied whole; the zero chunk before
      0xb3320000 keeps it 0x100-aligned; first mip level only, as the game's own logos/thumbnails) for
      CARSELECT_MANUFACTURER_HONDA (into the Miura's LOGOTEXTURES.BUN, which it replaces) and 4 vinyl thumbnails;
      vinyls_add (CARS/VINYLS/VINYLS.BIN: 0x8003ce00 chunks on 0x800 boundaries, key at +0x20); binary_parts +
      parts_add_rows (Binary "gMpB"+RAWW DBModelParts export -> rows of model VECTORVINYL; attribute kinds in
      PART_TYPES, checked against the game's MANUFACTURER_NISSAN and NIS_370 2TONE1 rows: text kept as bStringHash,
      or as u16 offsets/4 into the parts' string table 0x34606, 0xffff = none); VINYL_PART_MANUFACTURER_HONDA text.
      launch.c overlays CARS\VINYLS\VINYLS.BIN and FRONTEND\VINYLTHUMBNAILS.BUN (mod_addon_cars and mod_porsche_pack,
      since both bundles carry the rows). package.py removes ModData/addoncars/{converted,made,packed} after copying.
  * 2026-11-21 (the person: H doesn't end the chase; add Trueno, Lexus GS430, Subaru WRX STI, two Ladas; Russia tab):
    - H BUG: kit mods_guest_call accepted only main-image functions (recomp_index_of) and IAT entries, so core.nfsu's
      call of the plugin's FUN_1013ae90 returned POP_E_NOSYMBOL silently ("state 3 -> 3" in the person's log, six
      times) - every guest_call into PoliceCareer.asi had failed (also NFSU_PC_LISTCHECK). Now aux-module functions
      (recomp_module_lookup) are allowed; core.nfsu logs "call ok/REFUSED, state a -> b", shows the message only when
      the state went to 0, and logs the state again 1.5 s later. Not yet seen in a real pursuit.
    - SHOP SECTIONS / RUSSIA (2026-11-21, done, seen in the shop: Россия -> Lada 1200s / Lada Samara, buy dialog):
      the shop's "Америка / Европа / Япония" are rows of the game's static menu tree 0x00d2c140..0x00d2d52c (20-byte
      rows {id, parent, label hash, 0, 0}; ids 0xcd/0xce/0xcf under 0xcc). FUN_00597340(parent, list) builds a node per
      row (FUN_006a1d30(0x2c, "StandardFETreeDataNode" 0xbf5a60, [0xd3bd9c]) + thiscall FUN_005972b0(row)) and links
      it. The shop object ([0x00d9a4a0], ctor FUN_00581750) has four car lists (vectors of 36-byte entries: +0x10
      region, +0x18 career row id) at +0x48/+0x58/+0x68/+0x78; FUN_005b5010 (run when the shop opens) fills the first
      three by region with FUN_0052f570 (thiscall push) and then builds each section's children from all four; the
      choice handler FUN_00575d30 already has a case 0xdb for +0x78 that no menu row names. core.nfsu (addoncars.txt
      "region 3 <label>" + "regioncar <pv>" lines, written under "in addon": only with mod_addon_cars on - checked, the
      section is gone with it off): REPLACE FUN_00597340 links {0xdb, 0xcc, REGION_RUSSIAN} in before Japan (node id
      at link + 0x14; Америка, Европа, Россия, Япония - alphabetical, the person's wish); a
      BEFORE hook on FUN_0052f570 sends European pushes of those cars (their frontend region stays 1) to +0x78 by
      changing ECX; the choice hook clears byte +0x1f2 that case 0xdb sets, and moves any left in Europe.
      Dead ends: FUN_00591180 / FUN_00568790 "GetRegions" (other screens), FUN_0058d780 (garage groups), region 3 in
      the frontend (FUN_0051d7c0 then marks the row 0x80 "not for sale"), callsite hooks inside FUN_005b5010's loop
      (never fired). Diagnostics kept (env): NFSU_TEXT_WHO_TEXT=<hex text bytes> (who gets that text back - found the
      caller 0x00597307), NFSU_APT_TRACE=1, NFSU_SHOP_TRACE=1. Shop path for tests: pause > Машины > Down > Магазин.
    - NO SECOND COPY OF AN OWNED CAR (2026-11-21): the game itself lets you buy an owned car again - its CarSelected
      native (0x00569b40 -> FUN_005678c0, thiscall (shop, index into [shop+0x88])) checks only the garage size
      (FUN_00556ff0), money (value 6006) and "on sale" (FUN_0051df10: value 6001 of the frontend key, or class
      0xa13753eb's flag), never ownership; "КУПЛЕНО" is the list's isOwned (FUN_005913a0): query {vtable 0x00be9ad0, 0,
      frontend key, result byte +0xc, 0} run by FUN_00527c50 (fastcall). core.nfsu (addoncars.inc shop_owned_install,
      always on; NFSU_SHOP_OWNED_OFF=1 = old) asks the same query in a REPLACE hook on FUN_005678c0 and answers an owned
      car with SOUND_ACCEPT_DISABLED (0x00bf71e8 via FUN_004b3ed0 + FUN_00836ff0 on [0x01333770]) instead of the
      dialog. Shop mode [shop+8] == 2 (price 0, its own dialogs) is left alone. Not the unlock-all flag, not the cash
      top-up (tested off). FUN_005b5010's first loop does fill +0x78 (from query 0x00be7394, empty in practice);
      +0x1f2 makes CarSelected show dialog 10 for that list.
    - EXTRA OPTIONS KEYS ONLY WHILE DRIVING (2026-11-21, dist/pics/23): on the world map 9/0/- are the map's own keys
      (job filter) and still flipped my switches; "-" froze the camera ([0x00d89950]), so the car drove out of view
      and the garage menu showed the sky - the earlier "dead hang" was most likely this. eo_driving_now(): state 6,
      no pause, FUN_00521960 = 0 AND the world tick 0x00d3c108 moved in the last 300 ms (map and menus stop it).
      Presses outside driving are dropped (logged); the camera freeze is held only while driving and ends (switch
      off) when driving stops. Key presses are logged ("core.nfsu: key NN down ...").
    - RACE DIPS (2026-11-21, the person's race with rivals: 83-105 fps, MacBook hot): recorded with
      /tmp/racewatch.sh (RECOMP_D3D9_STATS, RECOMP_FRAME_TIMINGS, per-thread CPU, IOAccelerator GPU %, `sample` every
      40 s; /tmp/sparse.py busy-per-thread, /tmp/sincl.py inclusive). Main thread 94% busy; FUN_0076d720 (one vertex
      of a screen quad: HUD/menu sprites and text, six per quad from FUN_007816d0, a 6-row look-back de-dupe with
      CVTTSD2SI compares) was 15-19% of it. Native nfsu_vertex_add (byte-exact incl. D3DXVec4Transform through the
      new shared runtime/d3dx_math.cpp recomp_d3dx_vec4_transform, FLD/FSTP quieting sNaNs, EAX/ECX/EDX as left;
      NFSU_NATIVE_CHECK: 12M calls, 0 differ; NFSU_VERTEX_OFF=1 = translation). Race A/B (/tmp/racebench.sh, I-20 race
      on the person's profile, vsync off, no limit): 85-113 -> 95-131 fps at the same draw counts. The heaviest
      stretches (3000+ draws) are ~95 with GPU 8.4-8.6 ms/frame: next costs are main-thread scenery visibility
      FUN_0082a7d0 (~10% self), FUN_0078bad0, and the GPU itself. The audio mixer thread is ~30% of a core in races.
    - SCENERY VISIBILITY (2026-11-21): FUN_0082a7d0 (a scenery instance in a view, called per instance per view per
      frame by FUN_0082acc0/0082ad10) builds an SEH frame + cookie before its first tests and most instances fail
      them: native nfsu_scenery_visible runs those tests (skip instance on 4 of 8 frames, layer mask [st+0x48] vs
      [inst+0x3d], box vs view via box_vs_planes/box_vs_view for views 0x1f..0x21 else the section visibility bit)
      and returns for the rejected; the rest goes to the translation. NFSU_NATIVE_CHECK: 26M rejected, 0 differ;
      NFSU_SCENERY_OFF=1 = old. Its share of the race's main thread 12.9% -> 6.4%.
    - RACE GPU (Metal System Trace, /tmp/racegpu.sh; person's settings: scene 1512x982 4x MSAA, i.e. render scale
      half + MetalFX): ~82% busy; main scene c5 ~25%, the 2x-enlarged shadow map 6144x2048 (draw_distance adds
      3072x1024:2) ~19% (vertex 70 + fragment 87 ms/s), the 2x reflection maps 512x1024 with their mip passes ~18%,
      full-size back buffer (HUD) ~6%, presenter ~10%. The enlargements are mine (quality); not reduced unasked.
      Scripted race runs diverge (cutscene instead of race) - check frame dumps before comparing numbers.
    - PARALLEL VIEWS - NOT SAFE (checked 2026-11-21, the person asked to try): FUN_0082b070 runs FUN_0082afd0 per
      view state (0x60 bytes each, count [this+0x720]). Shared between views: (1) the occluder object 0x012f7f80 is
      REBUILT for each view by FUN_008257d0 (count +0x1800, +0x6850/+0x6854, globals 0x012f7e54/58) and read by
      FUN_00825b70 in every instance test - the address is an immediate in the code (MOV ECX,0x12f7f80 at 0082a89b),
      so per-thread copies would need per-thread code; (2) one output buffer 0x012e4d38..0x012f6678 filled view after
      view ([this+0x728] carries the end of one view into the next); (3) 0x012f7ea8 crowd flag. The render-info
      allocator FUN_00401900 is already atomic (LOCK XADD). Only the pure tests (now native, ~3% of the race's main
      thread) could run elsewhere - not worth the risk.
    - PER-DRAW COSTS (2026-11-21, the person's three points): (1) native FUN_00541730 (material layers bind, the
      render thread's top function): the whole listing natively, calling FUN_00540c40 / FUN_005410e0 / FUN_007674e0
      through recomp_call with the listing's stack frame, argument blocks, continuation addresses (005417fd,
      00541855, 005418e7, 00541926) and registers; [0x00d2ac0e] is 1 in .data and never written (the "always set up"
      branch is dead); native FUN_007674e0 (set a stage's texture) for the unchanged case. NFSU_MATERIAL_OFF=1 =
      translation. No per-call check mode (the callees have device side effects): verified by frame dumps only.
      (2) d3d9_metal.mm set_constants: fixed per-stage double buffer instead of vector assign + full clear, and a
      block equal to what the encoder already holds is not sent again (reset on a new encoder and by the clear
      quad's fragment index 0); RECOMP_D3D9_CONST_RESEND=1 = old. (3) runtime/cpu.cpp: aux modules get a range table,
      a continuation bitmap (recomp_module_is_call_return) and a direct-mapped index cache (module_index);
      RECOMP_MODULE_SLOW=1 = old. Driving A/B (/tmp/drivebench.sh, PQ profile, vsync off; scenes differ run to run):
      ~3-5% less work per draw on the render threads; recomp_module_is_call_return gone from the main thread's top.
    - RUSSIA vs BONUS CARS (2026-11-21, the person got a reward car): 0xdb and the shop's fourth list (+0x78, nodes
      +0xa4) are the game's own "Призовые машины" (FUN_005bb910 adds that row when a reward car is owned; case 0xdb
      sets +0x1f2 = cars are given, price 0, "Добавить"). Russia now has its own id REGION_ID 0x4db (not in the static
      tree) and its own lists: a REPLACE hook on FUN_005b5010 (after call_next) moves the Russian cars' entries out of
      +0x78 into g_rvec/g_rstore and their menu nodes (link = node+4, index at link+0x14) out of +0xa4 into the
      sentinel g_rlist, renumbering both sides; the choice hook does case 0x4db like the game's cases (FUN_00551dc0(
      [0x00d99354], 0x28) menu object, FUN_0056f520(list, 0), +0xac 0, +0x88 vector, +0x1f3 1) with +0x1f2 0, and
      leaves 0xdb to the game. Seen: Russia = Lada 1200s, Samara, Marussia; Призовые = Audi TT only, 0 $, "Добавить".
    - MENU HANG (2026-11-21, the person: "left in the menu, eventually it freezes for good"): their log (16:05) stops
      ~30 s after the free-roam pause menu opened (no frame-pacing / scheduler lines after; audio sink idles; window
      kept focus). A 15-min pause-menu test (/tmp/menuhang.sh, silent) did NOT reproduce it. launch.c now starts a
      watchdog shell beside the game (same pid, posix_spawn): no new "frame pacing" line (printed once a minute from
      Dev_Present) for 130 s -> a note in the log + `sample` into Logs/hang-<time>.txt, twice at most per run;
      RECOMP_NO_HANG_WATCH=1 off. NEXT: read the person's hang-*.txt when it happens again.
    - PLAY JOURNAL (2026-11-22, the person: "fps, heat, what lowers fps, every key, every action of the game, the
      closing time - everything"): Logs/<stamp>.journal.txt beside each log (launch.c sets RECOMP_JOURNAL and writes a
      header; RECOMP_JOURNAL=0 off; 10 kept). Writers: kit runtime/kernel32.cpp recomp_journal (exported, mutex,
      local time to ms) + the game's OutputDebugString text ("game: ..."); kit/mods/loader.cpp every mod log line
      ("mod ..."); host sdl/main.cpp keys with hold time, mouse buttons, pad buttons/triggers/connect, window state
      (trace_state), quit; host present_thread.cpp a "perf" line a second (game fps, shown fps, median/p95/worst
      interval, late frames, game GPU ms/frame, process CPU %, NSProcessInfo thermal state via host_thermal_state in
      metal_surface.mm, screen Hz, limit); d3d9_metal.mm report() a "render" line every 300 frames (draws, passes,
      render thread ms, GPU ms, waits) even without RECOMP_D3D9_STATS; boot.cpp the clean close; core.nfsu
      journal_pass (game state changes, world running/stopped, loading start/end with 1.5 s hysteresis, a "main" line
      a second: passes, longest pass, passes over 10 ms). The launcher's watcher shell (was the hang watchdog) adds
      "system" lines every 5 s (game CPU, GPU busy from IOAccelerator, memory, pmset thermal warnings), "dip" (perf
      under 85% of target 3 checks running while driving -> 1 s `sample` into Logs/dip-<time>.txt, 2 min apart, 10
      max), "hang" (as before) and the end ("closed: ... WITHOUT closing itself" when no clean close line). KEY_SCRIPT
      test presses don't pass the SDL key path, so scripted runs show no key lines. ~10k lines an hour.
    - THE TAB HANG - FOUND AND FIXED (2026-11-22, the person's journal: Tab while driving, world stopped, "loading
      started", then 0 fps and ~107% CPU until force-quit; the hang watcher's 130 s was too late). Reproduced with
      /tmp/tabhang.sh (person's profile copy, Tab after driving off; watcher samples when the journal's "main:" lines
      stop 8 s). Tab here starts the nearby job (a mission intro, then "Доберись до разбора"), loaded by job 0x00888280
      on the deferred-job queue FUN_006aec60: FUN_00881e80(0x013383c0) runs the loader state machine FUN_0087fc70 with
      a 150 ms budget and the job re-queues itself (FUN_0069e0b0(fn, arg, 1) -> next-pass list) until done. NFSU_QUEUE_
      TRACE=1 showed it re-queued every 5-15 ms with no main-loop pass ending (the main loop's command ring kept going:
      FUN_006ae4c0 -> FUN_00544cc0 -> 00679850 -> 00669c70 -> 006aec60) and every other guest thread waiting for its
      turn: the loader waits for the Job Manager threads, which never got the baton (cooperative scheduler; the main
      thread never blocked). Fix: core.nfsu BEFORE hook on FUN_00881e80 with ECX 0x013383c0 -> yield_baton (the game's
      Sleep(0)); 0x00888280 itself can't be hooked (reached only through a pointer, not a listed function - hook_install
      refused it). NFSU_MAP_YIELD_OFF=1 = old. Checked: 16 Tab presses, no stall, the job intro and the job started.
    - SHORT DIPS IN A MISSION (2026-11-22, the person's journal after the Tab fix: minutes at 117.3-117.6 fps, two
      1-2 s stretches at ~80 at 11:18:54 / 11:19:22): main passes over 10 ms with LESS process CPU (waiting), GPU 4-5 ms,
      heat normal, "76 new pipelines" in the log. d3d9_metal.mm pipeline(): every ask for a pipeline still being made
      waited up to 2 ms, so each draw needing it waited every frame. Now only the first ask waits
      (RECOMP_D3D9_PIPELINE_WAIT_ALL=1 = old). The journal's render line counts new pipelines/shaders. A/B with the
      ShaderWarm cache removed (pkgrun2 NOWARM=1, /tmp/psoab.sh): driving min 93.8 -> 107 fps, seconds under 100: 1 -> 0.
    - PAUSE-MENU HANG - FOUND (2026-11-22, the watcher's Logs/hang-20261122-115347.txt; Escape -> pause menu, 8 s at
      45 fps, then 0 fps, ~16% CPU = waiting): the game's RenderThread sat in Dev_Present -> host_d9_present ->
      RenderThread::sync waiting for an empty queue; the "d3d9 render" worker slept in work_cv_; the main thread waited
      for the render thread (FUN_00769460). Kit d3d9_host.cpp: the worker runs one batch behind (lag_) and only drains
      early while drain_ is set; it clears drain_ when the queue empties - a batch queued in that moment by another guest
      thread stayed alone, the worker waited for a second one and sync() for an empty queue: both forever. Fix:
      sync() counts itself in sync_waiters_ and the worker's wait drains whenever anyone waits there. Checked: 6 min in
      the pause menu, no zero-fps second (the hang itself was not reproduced on demand). The earlier 16:05 menu hang
      (log only) looks like the same.
    - RUSSIAN TEXT CODE: Russian_Global.bin uses the game's own single-byte Cyrillic order (not cp1251/UTF-8): a=82
      o=81 и=83 е=84 н=85 т=86 р=87 с=88 в=89 к=8b п=8d м=8f з=90 ы=91 я=92 ь=93 г=94 й=97 ч=99 ш=a6 Р=a8 С=a5 Е=a3
      А=9b Н=9d Т=9c Г=b7 М=af П=a2 Я=d5 (from its labels vs English). "Россия" = a8 81 88 88 83 92.
    - NEW CAR FORMATS: VLTInstaller.nfsms + files/data.end + DRAGnDrop (Trueno, Lexus): CARNAME labels, CarTypeInfos/
      Collisions/DBModelParts copy sources, or "import override ... DBModelParts <Binary export>" = the car's own rows
      (parts_add_rows now adds a new part model and re-sorts; PART_TYPES learnt d90f9423/643dabeb/2850a03b u32,
      46b79643 str, fe613b98 str2, 0x4b8 hash). Replacement mods inside the VltEd-install format (Ladas: file_copy to
      CARS\<game car>, no copy_node pvehicle) are converted (CONVERT keyed by the replaced car type: LAD_SAMARA_04,
      LAD_1200S_1980, same name lengths) and never copied over the game's car. Ladas: frontend russia (copy of europe,
      region 3) -> lada (copy of renault) -> samara2114 / lada1200s.
  * 2026-11-22 (the person: F10 shadow/reflection quality, more speed, controller polish; money row):
    - F10 "Тени и отражения" (core.nfsu/shadow_reflection_detail, restart): 0 Quality = as before (256x512:2 reflection
      maps, 3072x1024:2 shadow map when draw_distance > 0), 1 Balanced (reflections 1x, shadow 1.5x), 2 Smoothness
      (both 1x). launch.c sets RECOMP_D3D9_TARGET_SCALE only when a rule is left.
    - Native nfsu_depth_key (FUN_00763ef0, draw sort key: three D3DXVec4Transforms through recomp_d3dx_vec4_transform,
      truncation, EAX/ECX/EDX as left): NFSU_NATIVE_CHECK 8M calls, 0 differ.
    - MONEY ROW: mod_eo_cash (int 0..999999999, default 10 000 000, step 1 000 000, unit "$", live, group extraoptions).
      extraoptions.inc: a changed value sets the cash exactly (value 6006), otherwise tops up to it while unlock-all is
      on. Kit settings_menu.cpp: on a number row digits type a new value (first digit starts fresh, Backspace removes
      one, digits past the maximum ignored), Return clamps and applies (request_apply) - no restart. settings_menu_draw
      keeps the selected row visible when its help grows (last_avail). Checked: typed 2500000 + Return -> "career money
      10000000 -> 2500000 (the amount was changed)". (Test md3 ended by its timer before its Return - not a bug.)
    - CONTROLLER (investigated, NOT done): nfs.exe imports XInput ord2/4/5 only - XInputSetState is never called. The
      force-feedback update FUN_0069ad30 is gated by FUN_007577c0 (a DirectInput device at [this+0x70]+0xc+dev*4);
      road effect FUN_0075a770 and impacts 007570b0/00757130 (150000 us) all need DI effect objects. Rumble = a
      DirectInput force-feedback emulation in the kit feeding SDL rumble: a separate project. PlayStation/Xbox button
      prompts and pad/keyboard switching need a test with the person's real pad.
    - TRACKPAD TAPS (2026-11-22, setting core.nfsu/trackpad_haptics 0 off / 1 light / 2 strong, F10 Controls page):
      kit/host/sdl/trackpad_haptics_mac.cpp clicks the Force Touch trackpad through the private MultitouchSupport
      (dlopen: MTDeviceCreateDefault, MTDeviceGetDeviceID, MTActuatorCreateFromDeviceID/Open/Actuate, ids 3/4/6 =
      weak/medium/strong, RECOMP_TRACKPAD_IDS) without a finger on it; host_trackpad_haptic(level, count, ms) even
      clicks, host_trackpad_impact(intensity, length) a decaying knock (close strong clicks spreading out and weakening;
      a new pattern replaces the playing one only when about as strong as what is left). RECOMP_TRACKPAD_LOG=1.
      Game side mods/core/nfsu/trackpad.inc: the game's force feedback is useless here (its update FUN_0069ad30 is
      not called without a DirectInput wheel), so its EVENTS are hooked at their constructors: ECollision 00667d60
      (info by value: +0x2c impulse N s decides the strength, ~30 000 = full knock; the "closing speed" +0x20 reads ~0
      in car-to-car crashes - the person felt almost nothing when it was in the formula), EPlayerShift 0066b130 (one
      per shift, player only), ESpawnExplosion 0066e1a0 (pursuit breakers: debris keeps spawning - one knock per smash,
      2 s quiet), EShowRaceOverMessage 0066d920, EShowTimeExtension 0066d9f0, EShowRaceCountdown 0066d8c0; menu sounds
      = FUN_00834350/00834250 (bank lookup by name hash h=h*33+c from -1) returning non-zero, minus rotate/slider/
      chyron/zoom. Crashes and breaks count only while driving (menu-background traffic sends ECollision too).
      tools/dev/nfsu_event_ctors.py -> mods/core/nfsu/events_table.inc (every E... event's constructor) for
      NFSU_EVENT_LOG=1. TRAP: the person played in my test windows (crashes "before the script drove") - check for
      gear changes before the script's Up before trusting a test's crash numbers. Timers not seen in a race.
      Pads: the same knocks reach an Xbox/PlayStation pad through api->rumble_pulse (kit host_pad_rumble_pulse ->
      SDL): tp_pad_pass (hud_pass, every main-loop pass, every 30 ms) lowers a decaying envelope (heavy motor = the
      body, light motor = the first 80 ms); gears/menus/timers short buzzes. Not tried with a real pad. Final tuning
      (tp10): impulse^0.6 / 30 000; hard hits 0.74-1.0 (20 clicks / 0.9 s), sign-like hits 2 weak clicks.
      ENGINE VIBRATION (setting engine_vibration, done 2026-11-22): the player's EngineRacer (vtable 0x00c1c86c; its
      shift FUN_007192e0 = slot 43 makes EPlayerShift only for the player, so the ECX of the shift running when
      EPlayerShift is made is the player's engine): +0x138 RPM, +0x174 throttle, +0x178 brake, tuning [+0x4c+0xe0]:
      +0xadc redline, +0xafc top RPM, +0xb10 idle; owner vehicle +0x2c, its +0xcc body, body +0xac = the car's
      position in the contact points' (sim) coordinates. Class = FUN_0051dd30(fastcall career row of
      pc_active_car) = the car lot's tier (test car: class 4). Pulses on the throttle: rate and strength with RPM,
      rev limiter buzz, class 1 x1.0 ... class 4 x0.52. Crashes count only within 5.5 m of the car, breaks within
      8 m (the person: not other people's crashes); the break position's space (sim or render) was not settled, both
      are tried. NFSU_ENGINE_DUMP=1 logs the engine fields.
    - 2026-11-22 later (the person: vibration not alive, no class difference; two rows; money row a slider that did
      nothing; police helicopter):
      * Engine: per-class feel table in trackpad.inc (class 4 only weak clicks, slow, uneven, 30% beats left out;
        class 1 fast, strong, a double hit per beat high up), strength x (0.55 + 0.45 x pull) where pull = the RPM's
        climb per second / 2500 (pulling vs cruising). Gear change: the beat cuts out 160-220 ms, upshift a firm
        double thump, downshift a quick triple blip. A crash knock silences the engine until it has died away
        (g_tp_engine_quiet_until) and the host never lets plain clicks cut a knock still felt (g_playing_impact).
      * Rows: "Вибрация: удары, разрушения и передачи" / "Вибрация: двигатель при разгоне" (Controls page).
      * Kit F10 ENTRY FIELD: a setting with a unit (money) is a field with a caret while typed and its own Apply
        button (MenuLine.entry/typing, HIT_ENTRY / HIT_ENTRY_APPLY, MENU_POINT_EDIT / MENU_POINT_APPLY_ROW; Return or a
        click edits, digits append, Backspace removes). Money (extraoptions.inc): the row SHOWS the career's money
        (settings_set every 0.5 s; only a change this code did not write is the person's), a typed amount sets it
        whatever "Everything unlocked" is (the old code set money only with it on - why the person's change did
        nothing); with it on, topped up to the last amount set. Checked: row showed 9000, typed 3000000 applied.
      * HELICOPTER (not started): model CARS/COP_HEL_FED_01; the game's own AIVehicleHelicopter (AIGoalHeliPursuit/
        Patrol/Exit), camera ECameraHeliCopCam, CSTATE_Helicopter. Plan: spawn it at the car, steer by moving its AI
        target from the keys, its cop camera, exit on car change, arrest by hovering over a racer.
      * POLICE HELICOPTER (mods/core/nfsu/heli.inc, setting police_heli 0 off / 1-5 which player police car; done
        2026-11-22 after dead ends):
        - DEAD ENDS: an AI heli from the cop manager ([0x00de6dac] slot 8 FUN_0043e1e0, "cop_heli") is created but its
          model (CARS/COP_HEL_FED_01) is never loaded outside a pursuit - invisible; making the player's car pvehicle
          cop_heli (career row +8) loads the model but crashes in car code (null method from 0x0043416a).
        - WORKS: the player's own police car with the helicopter's MODEL. pvehicle MODEL is a StringKey {hash, name}
          (type a502a824; cop_hel_fed_01's = 6185b54f): copied into the chosen PLAYER police collection
          (player_cop_car_suv_04 c1733734, _mid_05 1bb4725e, _mus_08 088eee9d, _911_tur_06 92dba446,
          pc_player_cop_car_por_06 eff67758 - the plugin's FUN_10135f20 keys; base-game collections, AI cops use
          cop_car_*) -> the car is drawn as the helicopter, rotor turning, the chase camera, HUD, pursuit as a car.
        - FLIGHT: each main-loop pass while flying, the player's RigidBodyPart (players' list [0x00de93d4] -> IVehicle
          (PVehicle+0xb8, vtable 0x00c19594) -> ISimable PVehicle+0x24 -> [+0x48], vtable 0x00c1d1a4) gets
          SetLinearVelocity (slot 23) from the keys, SetOrientation (28, yaw + speed pitch + turn roll; forward =
          rotation column 2, heading atan2(col2.x, col2.z)), SetAngularVelocity 0 (24). Havok still collides.
          Up/Down forward/back 45/15 m/s, Left/Right turn 1.3 rad/s, Space/E climb, Q/Ctrl descend 9 m/s; take-off
          Space on the ground; landing = descending while the ground holds it 0.4 s. Checked in dumps (I-20, PQ
          profile with NFSU_HELI_MODEL=1 = the active car becomes the heli for tests).
        - ARREST: the plugin's pursuit controller [0x101d69a0]+0x13e4 (state 2/3), racer = FUN_1014fe40 (fastcall
          ctrl+0x28) = IVehicle; the plugin arrests when racer vt+0xdc -> AI, AI vt+0xc0 -> pursuit, [pursuit+0x2a8]
          == 3 (FUN_10150560): the heli within 8 m across, 2-35 m above the racer for 2.5 s sets it. NOT tested in a
          real police pursuit (scripted runs can't reach police mode).
  * 2026-11-22 latest (the person: helicopter camera/borders/void/reset/controls; the Xbox pad ignored by the game;
    4 vibration rows; trackpad quiet with a pad; pad status and picture in F10 Controls):
    - HELI: camera = BEFORE hook on the look-at FUN_00749920 when called from the chase camera FUN_004deb10 (return
      0x004e0d20): eye/target/up replaced while flying (render coords x = sim z, y = -sim x, z = sim y; eye 16 m
      behind a smoothly following heading, 6 m up). Resets refused while flying: FUN_00720310 (vehicle reset to a
      position, RET 0xc; called from the last-good-position ring FUN_007373d0 and FUN_00730050; raises
      EVehicleReset) - at 273 m the game had put the car back. A stopped world tick (streaming while flying fast) no
      longer ends the flight. Walls: <25% of the asked distance over 0.3 s -> SetPosition 2.5 m on. Ceiling 160 m
      over take-off. Height hold (gravity between Havok steps sank it 0.5 m/s). The void below the sky: the fog pass
      (961f patch) gives far, black pixels the fog colour (r[2]) - the black was the scene clear showing where nothing
      is drawn; kit d3d9 recomp_d3d9_set_void_colour (clear override) exists but did not cover it. Back in a car:
      EResetPlayerCar (factory FUN_0066c340) puts it on the nearest road. F10 row police_heli removed (the police
      SUV is the helicopter whenever police mode is on). NOT done: the helicopter as its own 11th car (the plugin's
      police set is hard-coded in ~11 functions: FUN_10134ca0, 10135880, 10135970, 10135f20...), its own sound, a
      helicopter icon on the map.
    - PAD: the game finds pads ONLY by DirectInput enumeration (FUN_0075c680 callback FUN_0075c190 skips a device
      Microsoft's WMI IsXInputDevice FUN_00756ab0 calls XInput - WMI is absent here), then FUN_0075e8f0 reads its
      VID/PID (GetDeviceInfo guidProduct) and FUN_0075e150 looks the VID/PID up with SetupAPI
      (SetupDiGetClassDevsA/EnumDeviceInfo/GetDeviceInstanceIdA); an instance id with "IG_" makes FUN_0075d8b0 drive
      it through XInput (object vtable 0x00c2084c). game.toml dinput = true (was false: no pad at all), launch.c
      RECOMP_DINPUT_XINPUT_PATH=1 -> kit dinput_joystick guidProduct VID 045E PID 028E, kit misc.cpp SetupAPI with one
      device "HID\VID_045E&PID_028E&IG_00\...", dinput.cpp DIPROP_GUIDANDPATH. Checked with the new RECOMP_PAD_SCRIPT
      (kit controls_host.cpp: "SEC:cross|r2|lx-...:HOLD"): XInputGetState polled, A goes through the menus, X = gas in
      this game's default layout (R2 did nothing), stick read.
    - host_pad_kind(name, cap) (kit gamepad_sdl.cpp, exported): 0 none, 1 Xbox, 2 PlayStation, 3 other;
      RECOMP_FAKE_PAD_KIND=1/2 for menu tests. F10 Controls page: SRC_PAD line (MenuLine.pad_kind) "Подключён
      джойстик: <name>" with a drawn pad (Xbox / PlayStation marks and colours) and a legend of the checked actions.
    - Vibration rows: trackpad_haptics / engine_vibration (trackpad), pad_haptics / pad_engine (controller); the
      trackpad is quiet while host_pad_kind says a pad is connected.
  * 2026-11-22 night (the person: pad can't drive; heli left/right swapped, through buildings/mountains but not
    invisible walls, a grey void instead of sky, the car falls from the heli's height):
    - PAD DRIVING: the PC game has pad defaults only for menus. Its input table (TInputSpec, FUN_006774c0 from the rows
      at 0x00d37f98, 0x34 bytes: name, keyboard primary/secondary, pad primary/secondary; runtime [0x00da39a4 + 8*i],
      12-byte specs {type, 0, code}, pad j at +0x18 + 0x18*j) left nitro on button 6, which an Xbox pad never gives.
      Types (FUN_00677be0): 2 button (X 0, A 1, B 2, Y 3, LB 4, RB 5, Start 9), 3 POV, 4/5 X/Y/Z axis low/high half,
      6/7 Rx/Ry/Rz, 8/9 sliders; a half = max(0, +-(32767.5 - v)) / 32767.5. The pad state the game reads ([0x00da38e0]
      vt+4): LT -> Rx (+0x20c), RT -> Ry (+0x210) = (t - 127) * 514 from the XInput byte. core.nfsu padlayout.inc
      (setting pad_layout, 1 console default): RT gas {7,0,1}, LT brake {7,0,0}, stick steer, A nitro, B
      speedbreaker, X handbrake, Y look back, RB camera, LB reset; rechecked every second. Kit xinput.cpp
      recomp_xinput_remap("src=dst,...", trigger ranges "rt@lo:hi") + recomp_pad_raw (exported): core.nfsu sets
      "lt=lt@190:255,rt=rt@190:255" so the axis's high half spans the whole pull (analog; measured with the new pad
      script names l2h/r2h). Checked: RT gas, LT brake/reverse, stick, B Speedbreaker event; nitro (A) not seen
      firing in a test (gauge state unknown). Diagnostics: NFSU_PAD_DUMP="s,s2,..." (devices, bindings, specs,
      pad state), NFSU_EVENT_LOG logs NOS/SpeedBreaker every time.
    - HELI: turn sign fixed (Right turns right); pad flight (left stick, RT up, LT down; take-off with RT); blocked
      for two 0.3 s windows -> climbs 6 m/s over it (no more stepping through buildings and mountains). Invisible
      walls: the game's barrier lines (bounds flag kFlag_NoCollision_Barrier 0x40 exists; where it's checked not
      found) - they still stop the heli unless it climbs over them. Void below the sky: 961f patch copies only real
      sky (TEXTURE2 depth dot c7 >= 0.9999, not black) mirrored about the exactly found edge (6 halvings; the first
      try with 1/64 steps copied in layers and copied buildings), else the fog colour. Back in a car: the game's reset
      runs on the first driving pass once a new body exists (was 1.5 s later, the car fell meanwhile); the car is
      held still until then. Measured: the reset itself puts the car on the road at road height. Dead ends:
      FUN_007302d0 (road snap, 6 m window) never found a road from above; pressing the car down at 45 m/s sank it
      under the world.
    - The person's real Xbox pad is often in their hands during tests: stray pad input (take-offs, stick values)
      shows up in test logs.
    - Open (the person's): streaming can't keep up when flying fast; the heli acts as a car in pursuits (cops chase
      you, map shows the player) - not started.
  * 2026-11-23 night (the person: car still falls after the heli, heli has the car's sound and no heli map icon,
    invisible walls stop it, far streaming holes/strips; liked the sky):
    - Car after the heli: leaving police mode reloads the world (11 s in their log) and my 8 s wall-clock wait gave
      up before driving resumed. Now the wait counts driving only: reset (EResetPlayerCar) when a new body appears or
      after 1 s of driving; the car is held still meanwhile. NFSU_HELI_LEAVETEST=<s> simulates leaving; the log says
      where the car stands 1.5 s after the reset. Checked: on the road at road height.
    - Heli sound: pvehicle BEHAVIOR_MECHANIC_AUDIO (behaviour factories registered by name, 0x00bc5260..: SoundCop
      FUN_00736da0, SoundRacer FUN_00736fc0, SoundHelicopter FUN_00737150) copied from collection "choppers" into the
      host car's own field table. The tables are sized full by the game: heli_attrib_give grows one (2n+3, every node
      rehashed: rotated key % count, linear probing, probe count in the home slot +0xa, used +6). Checked: the
      SoundHelicopter factory runs for the player car, no crash; recorded audio changed (centroid 561 -> 1358 Hz,
      a ~9 Hz pulse). NFSU_HELI_NOSOUND=1 / NFSU_HELI_SFXLOG=1.
    - Far distance from the air: kit recomp_d3d9_ps_reg_set(code key, reg, x,y,z,w) (d3d9.cpp, exported) gives a mod
      a live pixel-shader register; the 961f patch adds haze toward the game's fog colour by view distance
      (c8.w / (depth + c8.x), as the shader's own fog) with c20 = (amount by height 15..90 m, start 260 m, end 560 m;
      NFSU_HELI_HAZE=start,end). Void fill: sky only at depth >= 0.99999 (far terrain ~0.9998 was copied in stripes),
      mirror averaged over 5 points; else the fog colour. The streamer itself was not changed (no lever found).
    - Invisible walls: held 2.5 s while climbing 12 m/s -> moved 6 m on (log "through an invisible wall"). The world's
      edge has no wall high up: the heli can fly out over the void.
    - Minimap: AI vehicles get HELICOPTER_MINIMAP_ICON (binhash 0x586874a0) in FUN_005cca20 when their class equals
      [0x00de6ef8] (CHOPPER); the player's arrow is drawn by the APT HUD movie - not changed.
  * 2026-11-23 (the person: the heli must not touch invisible walls at all): the 2.5 s step-through is gone. The
    game's Havok collision filter (hkNFSCollidableManager, subobject vtables 0x00c17fb8..0x00c17ff0; decision
    FUN_006d7bd0(infoA, ownerA, infoB, ownerB): low 6 bits layer, jump tables 0x006d7d28/48/64/7c/90) always lets a
    car (layer 3) hit the world (layer 6). Layer-1 bodies use the top byte as kFlag_NoCollision_* (0x20 ground vs
    world sub-type 0, 0x40 barrier vs world sub-type != 0 in bits 6..9, 0x80 cars). Measured (NFSU_HELI_FILTERLOG=1):
    world shapes are sub-type 0 or 1. The heli's body info ([[rb+0x24]+0x2c], hkpCollidable +0x1c) becomes
    (info & 0x00ffffc0) | 1 | 0x40000000. Checked at the I-20 start: old info 00000003 stuck at x -1067 (could not
    even climb); new one flew 1000+ m at 4-5 m and still climbed over something solid. NFSU_HELI_WALLS=1 = old.
  * 2026-11-23 (pics/24: the camera jumped to the heli and back passing a barrier; a ladder under the heli over the
    void; the world unloaded far from roads): after the look-at the chase camera FUN_004deb10 runs FUN_004ca370
    (at 0x004e0dad, RET 0x10, its own look-at at 0x004ca43f, result -> [cam+0x151]) and FUN_004d1bd0 (at 0x004e0dd9,
    RET 0x14, look-at again at 0x004d2097) - the camera-blocked pull-in, past our override. Both are REPLACEd to
    return 0 while flying (checked in frame dumps under the I-20 start overpass: steady camera). Void fill: the edge
    is the first FAR (depth >= 0.99) non-black pixel above, near things looked past (the heli's thin parts gave the
    ladder). Streaming far from roads not changed.
    - Car blob shadow under the heli (the person: remove the wide static shadow): FUN_00851d50 (thiscall, ECX the
      car render info, arg 2 the car's position in render coordinates, RET 0x1c) draws CARSHADOW [info+0x444] and
      WHEELSHADOW [info+0x448] (set in FUN_0087a230) when the texture is non-zero. BEFORE/AFTER hooks zero both for
      the draw within 6 m of the heli (measured 3.5 m off: the position is the body's base) and put them back.
      Checked by log only ("its car shadow is not drawn"); NFSU_HELI_SHADOW=1 keeps it.
    - OPEN (the person, 2026-11-23): "the whole map sector the camera looks at, and a bit beyond, must load at once"
      - the TrackStreamer, not studied yet. The person may be at the Mac in the morning: their pad input showed in
      test runs and test windows pop up over their screen.
  * 2026-11-23 STREAMING FOR THE HELI (mods/core/nfsu/stream.inc; the person: everything on screen loaded before
    they look at it): track streamer 0x01317f30 (section rows [+0]/[+4], 0x5c bytes: +8 number, +0xb wanted bits,
    +0xc state 4 loaded, +0x18 size; wanted list +0x650, count +0x64c, ROOM FOR 512 ONLY - FUN_00828570 doesn't check,
    611 entries overwrote the pool pointer at +0xe50 and hung the game twice); visible-section manager 0x012fe7e0
    (zones: lists +0/+8, node +8 number, +0x18/+0x20 2D box in render coords; load lists +0x10: +0xc zone, +0x12
    count, +0x14 shorts). The game loads only the zone under the car's list (none off-road: FUN_00829110 distance
    limit) - 4 MB while flying. While flying, every 0.5 s: zones within 450 m, or within 1400 m and within 75 deg of
    the camera's forward, their lists joined nearest first, within 70% of the pool, added by an AFTER hook on
    FUN_0082a410 up to 500 entries in all; FUN_0082e1e0 refreshes when the set changes. The pool is 256 MB of its
    own (VirtualAlloc through IAT 0x00bd61b8, passed to FUN_0082a020 in place of the game's 160 MB block; the mod
    heap is ~15 MB). Measured: 586 sections / 175 MB picked, no hang, same fps. Env NFSU_STREAM_OFF, _RANGE, _LOG,
    _POOL_MB (0 = 160), NFSU_STREAM_DUMP=<s>. The hole right of the I-20 hills is the world's edge (nothing to load).
    - DRAWING from the air (pics/25: half buildings, missing roofs while hovering downtown): per view FUN_0082afd0
      draws only FUN_0082ae10's list (STDCALL (short *out, max 128, view), RET 0xc - a cdecl return gave
      0xC0000409): the always-on 25000.. plus the loaded sections of the load list of the zone under the camera,
      i.e. the ROAD's view. While flying stream.inc REPLACEs it: 25000.. then the loaded sections nearest the heli
      (box [[0x012fe83c+4n]+4]+0x2c, min +0 / max +0x10, render coords) not more than 60 m behind the camera, up to
      128 (the caller's buffer; FUN_00401900's render-info pool writes to address 0 when full, so not more).
      Downtown test (NFSU_HELI_TELEPORT=1295,70,-1146@72): the far island now drawn (was a fog patch); a patch
      below-left stays. Limits measured: 339 sections wanted and loaded (the wanted list's 512 cap), only ~45 of
      them scenery with boxes in view. NFSU_STREAM_DRAW_OFF=1 = the game's list.
    - Queue filling (2026-11-23 later): a zone's list is ~150 sections - ~60 'A' detail sections (0.1-1 MB), ~40 'Y'
      (1-3 MB, shared by a district), small 'W'/'U' (NFSU_STREAM_ZONEDUMP=1 prints the nearest zones' lists). Our
      list used to repeat the game's own entries, so only 339 of the 500 places filled; the AFTER hook now passes
      only sections not yet wanted (row +0xb == 0; g_sec_row maps number -> row), into the buffer's second half.
      Downtown: 500 wanted and loaded, draw list 52 of the 128 places; the far coast and island now drawn; a pale
      patch below the tower at the left stays (no section covers it - probably never modelled).
    - CRASH 2026-11-23 12:26 (the person's log): right after "you are in a car" (leaving police mode reloads the world)
      the render thread's deferred-command list FUN_00791c40 (list 0x00d53bf8, commands from FUN_007976e0/00797780)
      called a freed object: call to 0 at 0x00791c61, then stack runaway. Cause (most likely): stream_pass forced
      FUN_0082e1e0 when the flight ended, unloading ~300 sections during the game's own reload. Now leaving the
      flight only clears our list/draw list (no forced refresh), and refreshes run only while tp_driving(). A
      simulated leave (NFSU_HELI_LEAVETEST) ran clean; the real police-mode switch not reproduced in a test.
  * 2026-11-23 THE HELI AS ITS OWN POLICE CAR (the person: not instead of the police SUV): career rows ([0x00d8e1a4]
    +0x88, 0x14a x 28 bytes: [0] id, [1] frontend = name/picture, [2] pvehicle = what is built, [3] preset) keep the
    frontend apart from the pvehicle, and the plugin's ~11 police checks look only at [2]. So the heli is a second
    row of player_cop_car_suv_04 with frontend "copcar" (f0ed1c9b, the game's AI police car frontend): made once
    per career by the plugin's FUN_10170840(rows, pv, preset 0) when driving, then [1] set (found again every pass
    by [1]+[2]); named "Полицейский вертолёт" (REPLACE FUN_00525070 for that frontend -> label 0x48454c49, REPLACE
    FUN_00571b80 answers it). heli_look_set: the SUV collection's MODEL and BEHAVIOR_MECHANIC_AUDIO value are the
    heli's only while that row is the active car (pc_active_car), the SUV's otherwise; g_fly_on = that row active.
    Checked: the row was made (career car 000000d8 on PQ) and the SUV stays an SUV; the police garage menu itself NOT
    seen (scripts can't reach police mode). Preset 0 on purpose: a second row with the SUV's preset is left empty by
    the game (FUN_00524d60), and FUN_101707d0 would mix the two rows up. The plugin's "on sale" check
    (0x10168aa0) falls back to the game's for this row.
  * 2026-11-23 (the person: the heli isn't in the police shop - put it at the bottom; pics/26: police SUV and patrol
    sedan camera too low and close):
    - Shop: FUN_005b5010 takes cars from the "for sale" query (vtable 0x00be7418, filled by FUN_00519850(store,
      query, -1), thiscall RET 8): vector query+8/+0xc/+0x10 of 36-byte entries {+0 frontend, +0xc price, +0x10
      section 0x10000/0x20000/0x40000, +0x18 career row id} (NFSU_HELI_SHOPLOG). BEFORE+AFTER hooks: when the police
      SUV is in the list (police mode), the heli's entry - or a copy of the SUV's with the heli row id and the copcar
      frontend - gets the SUV's section and goes to the END. Not seen in the police shop (scripts reach only the
      racer shop, where nothing changes).
    - Name hooks are AFTER hooks now (REPLACE on 0x00525070 was refused: the add-on cars' REPLACE sits there; the
      person's log said "name hooks NOT installed"); cdecl args are at esp+0 after RET.
    - Police camera: SUV (c1733734) and sedan (1bb4725e) get distance 145% and height 170% (field Height 0xba847865,
      same graph layout as Position 0x9f743a0e); Mustang/911 keep 115%/100%; re-applied when the active car changes.
      Not checked on screen.
    - The POLICE shop is the plugin's own (its log: "Police dealer query: 5 native car rows"): FUN_101365a0 (stdcall
      (rows, vector), RET 8; vector [0] begin [1] end) pushes its five stock cars (keys at 0x101a52ac) with
      FUN_101710b0 -> the game's FUN_0052f570 (thiscall vector, entry). BEFORE hook keeps the vector, AFTER hook
      pushes a copy of the police SUV's entry with frontend copcar and the heli row id the same way (last place). The
      game-query hook (0x00519850) stays for the racer shop but never saw police cars. Not seen on screen.
  * 2026-11-23 HELICOPTER IN THE POLICE SHOP (pause > Режим полицейского > Магазин > Служебные машины; same screen as
    Машины in police mode): the plugin fills the game's "for sale" query (vtable 0x00be7418, FUN_005b5010's first
    FUN_00519850 call, return 0x005b506f) through its FUN_101365a0 -> FUN_101710b0 tail jump -> the game's push
    FUN_0052f570 ([esp] = 0x10136646). The kit hooks only main-module functions (hook_install on 0x101365a0 was
    refused), so heli.inc hooks the push: the first push of a fill pushes the car itself, then the helicopter's entry
    (copy with +0 copcar 0xf0ed1c9b, +0x18 its row) takes that push; later pushes write the car into the helicopter's
    slot and push the helicopter again (it stays last). The shop then std::sorts each section (FUN_005a1b60, preds
    0x00556ee0 class / 0x005570a0 name - not hookable): an AFTER hook on the sort moves the helicopter to the end.
    The list asks the text of label 0x136e857f for the copcar frontend: heli_text answers it, written in the game's
    own Cyrillic code (л 8a, ц b2 found in "Кольцевая гонка"). Seen on a frame: "Полицейский верт..." last.
    Diagnostics: NFSU_HELI_SHOPLOG (pushes, entries, stack of the first push), NFSU_HELI_NAMELOG (labels, MISSING
    texts, the list's text bytes).
  * 2026-11-23 later (dist/pics/27: the helicopter showed the SUV in the shop; "can't buy it, garage full"; an old
    "helicopter" in the garage):
    - "copcar" (0xf0ed1c9b, HELI_ROW_FE) is ALSO the police interceptor's frontend: the global copcar -> helicopter
      label mapping (heli_name_label) named the person's interceptor row 0x10c as the helicopter - removed.
    - The helicopter's row (0x11f: copcar, SUV pv, flags 0x00080080, slot 0xffff) is NOT an owned row, so the game's
      owned-cars fill (push return 0x0052fad6) never put it in the police garage (object [0x00d9a4a4], list +0x58,
      index +0x80; it lists every owned row - the person has 30 = the game's garage limit (FUN_00556ff0, 0x1e) - and
      shows the police ones). heli_garage_push adds it (copy of the first police car's entry, row 0x11f) and keeps it
      last; choosing it there makes it the active car (checked: "you are in the helicopter", heli shown). It does
      not take a garage place. Its garage name is still the SUV's: the plugin names police cars itself (not
      through FUN_00571b80 - NFSU_TEXT_ALL=1 with NFSU_HELI_NAMELOG=1 logs every text lookup).
    - Shop "Добавить" on the helicopter: shop_car_selected (addoncars.inc) answers it with the disabled sound and a
      log line instead of the purchase (it is already in the garage) - no more "garage full".
    - Previews: BEFORE hooks on the highlight handlers FUN_005aa390 (shop), FUN_0058d530 (racer garage) and
      FUN_005aa740 (police garage, thiscall row) switch the SUV pvehicle's MODEL to the helicopter's while the
      helicopter's row is highlighted (g_fe_heli; -1 again when driving or no shop/garage object is open). Seen in
      the shop and the garage. Natives: CarHighlighted 0x005b8320 / 0x0059dd50 / 0x005b8370 (registration blocks
      at 0x005bcb12 etc.: push fn, wrap, push name).
  * 2026-11-23 evening (the person: "three police SUVs in the garage, two of them helicopters; sold ones come back"):
    - The SUV pvehicle's MODEL is one global value; the garage/shop build their previews asynchronously from it. With
      the helicopter as the ACTIVE car the look stayed on in the menus, so the garage built real SUV rows as
      helicopters. Now in a shop/garage (objects [0x00d9a4a0]/[0x00d9a478]/[0x00d9a4a4]) the look is on only while
      the helicopter's own row is highlighted; driving/loading: the active car decides. Checked on a frame walk of
      every row: row 0x120 (SUV) stays an SUV before and after the helicopter.
    - Row 0x11e (the person's other SUV, slot 0x1c) shows the helicopter even with the look off, on a fresh start:
      it carries it in its own saved data (left by the old version that swapped the SUV; the model key is not in the
      career object or any car record - NFSU_HELI_MODELSCAN=1 scans guest memory: only vault data has it). NOT fixed.
    - Selling a police car (garage "2" -> "Продать эту машину за 1650 $?" -> Да) gives the money and drops it from
      the list but does NOT remove its career row (NFSU_HELI_ROWLOG=1: rows 0x11e/0x120 unchanged), with or without
      the helicopter in the list (NFSU_HELI_GARAGE_OFF=1 = no helicopter entry): the plugin's police garage lists
      every owned police row again. Not fixed (the plugin's sale path not found; garage natives at 0x005bcb90:
      GetCarList 0x5917b0, GetCurrentToggle 0x5915a0, UpdateCarList 0x578be0, CarSelected 0x5a1350, CarHighlighted
      0x5b8370; no sell native - the sale is a dialog in C++).
    - Test key path changed (the profile's save list moved): Escape at 22.5 s, police mode = 3 Downs + Return,
      Гараж = Return, Магазин = Down + Return.
  * 2026-11-23 CRASH IN A POLICE CHASE (the person as the racer; log 20261123-165048): "call to 00000000 (return
    004239b5)" then EIP 0x3e800000. FUN_00423950 (an AI goal choosing its action) calls each listed action's vtable
    without a null check; a 0 entry came from FUN_004417b0 (adds an action by registry name, registry [0x00d82680],
    28 static registrations 0x00bbb100..), whose factories (0x004393d0 ...) return 0 only when the "Fastmem"
    small-block allocator FUN_005f6340 (object 0x00da5320, 1880 call sites) runs out ("Out of Fastmem memory", then a
    deliberate store to address 0 that the kit turns into nothing). Its region is fixed by FUN_0069e4c0: 0x271000
    bytes. Fix (aiguard.inc): AFTER hook on 0x0069e4c0 makes it 4x (10000 KB; NFSU_FASTMEM_X=n, 1 = game's) - logged
    "core.nfsu: AI: Fastmem pool ..."; plus a guard that takes a 0 action off the goal's list (logs its name;
    NFSU_AIGUARD_OFF=1). Likely trigger: Revamped Pursuits' bigger police numbers. Checked: boots and drives; a long
    pursuit not reproduced.
  * 2026-11-23 POLICE GARAGE SALES FIXED: the plugin's sale (key 2 -> "Продать эту машину за N $?" -> Да) only pays and
    redraws: the garage list ([0x00d9a4a4]+0x58) keeps its entries and the game's sale command
    (DALVehicleCommands::RemoveCustomizableCar, vtable 0x00be6e08, execute FUN_0052e010 -> FUN_00528da0(rows, row id,
    2, 0), which frees the row and its customization slots) never runs, so sold police cars came back.
    aiguard.inc police_sale_pass (each main-loop pass, 20 checks/s while the police garage is open): when the career
    money (value 6006) rises - not to the unlock-all top-up amount - the row highlighted ([obj+0x80]) in the pass
    before is removed with FUN_00528da0 (police rows only, never the helicopter's). Log "core.nfsu: police garage:
    car N sold ... removed from the career: yes"; NFSU_SALE_DEBUG=1, NFSU_POLICE_SALE_OFF=1. Checked: row 0x11e
    gone from the career after the sale, garage list one shorter on re-entry. Not checked across a save + restart.
    Row 0x11e (the person's helicopter-looking SUV from the old version) can now simply be sold.
    NFSU_SELL_LOG=1 hooks the game's sale command / removal / 0x0052d480 (none fire for police sales).
  * 2026-11-23 police garage polish: (1) the patrol sedan (pv 0x1bb4725e) can't be sold - key 2 on it is swallowed
    (on_key, disabled sound); a sale that still happens (pad) keeps the row and sets the money back. (2) Key 2 on the
    helicopter (its "УБРАТЬ" only hid it) is swallowed too. (3) Garage name: the plugin's garage filter (its
    FUN_101363c0, runs AFTER FUN_00519850 returns, at the caller) gives every police entry +0 = its frontend for the
    pvehicle, so the helicopter was "Полицейский внедорожник". heli_sort_after (the list is sorted after the filter)
    sets the helicopter's +0 to HELI_GARAGE_FE (0x48454c46, no such frontend); the garage builder FUN_005b6120 then
    asks label 0x136e857f at 0x005b622d, which heli_text answers (as at 0x005b54e2 in the shop). Seen: "Полицейский
    верт..." in the garage; choosing it still makes it the active car. Row 0x11e sells and is removed on the
    person's current save (checked).
  * 2026-11-23 GREY SCREEN + QUIT after turning mods off (Police Career, add-on cars off): the save's active car was
    0x1b5, a row that does not exist without the plugin (it keeps police cars in its own data); no player car was
    made and FUN_0042b650 called methods of a null car ([0x00de93dc] = 0) in a loop until the host closed it. Fixes
    (addoncars.inc): an AFTER hook on the active-car read FUN_00820c70 (key 0x307d9226) hands the game a real owned car
    (owned 240SX first, addon_pick_car) when the active car is an off add-on OR has no row while Police Career is off
    - at once, before the world asks (the 0.5 s guard was too late). And FUN_00821e30 ADDS its value to the stored one
    (0x1b5 + 0xd7 = 0x28c, every 0.5 s - the old add-on guard had this bug too): the guard now writes pick - id.
    Checked on the person's profile: active 0x1b5 -> 0xd7 once, the save drives. NFSU_ACTIVE_LOG=1 logs each read.
  * 2026-11-23 (dist/pics/28: arrest clothes "like coloured foil", see-through white holes under cars and in wheels):
    MY void fill in 961f166f1ae442c4.patch (2026-11-22, helicopter sky) took any near-black unfogged pixel for the
    void - r[1].y = 1 means "no fog", true for NEAR pixels as well - so black tyres, shaded undersides and black
    clothes were painted with sky/fog colour. The fill now also needs the depth at the far plane (hz_depth >=
    0.99999: nothing drawn). Checked: chase frame on the person's profile, the 240SX's underside and shadow clean.
    The arrest scene itself not reproduced (the 11-18 screenshot in pics/28 is older than the fill - if the foil
    stays, it is a different cause: the character shader's explicit-level reads, see 11-18 notes).
  * 2026-11-23 late, "FOIL" CLOTHES + BLACK EYES at arrests (dist/pics/29) - NOT FIXED, findings:
    - Reproducible arrest (unattended): profile "NFS Undercover PQ", MS="mod_eo_heat_hack=1,mod_eo_heat_level=5,
      mod_eo_cops=1,mod_eo_autodrive=0,mod_police_career=0,mod_addon_cars=0", KS="32:Return:0.1,45:Return:0.1,
      60:Return:0.1,75:Return:0.1,80:Up:25,106:Down:3", 200 s; the arrest cutscene is at presents ~13500-17000
      (dumps every 300 from 70 s). (The person's own profile now starts inside a race - not usable.)
    - The clothes/skin shader: code key 2a6ebd55f9cba503 (MSL dump a120be3d7063accf; RECOMP_D3D9_SHADER_DUMP=<dir>),
      ps_3_0 with vFace (cmp of vFace picks the normal's sign), stages: 0 = reflection target 20 (256x512 A8R8G8B8,
      texldl), 1 = shadow map (3072x1024 R32F), 2 diffuse DXT1/DXT5, 3 spec mask DXT1, 4 normal map DXT1 (.xy, z
      rebuilt), 5 misc DXT1. 50 of its draws use cull 2 (CW culled: visible faces are CCW -> vFace -1 -> flipped
      normal), 19 cull 1. Also new in the arrest: 14b283be46760bd7 (character shadow-depth pass), 8edbb3c48f86fb48.
    - Ruled out (foil unchanged): reflection samples replaced by 0.5; shadow_softness 0; draw_distance 0 (game CSM
      distances); vertex formats (RECOMP_D3D9_DECL_LOG=1, kit: characters use SHORT4N position/normal/tangent,
      SHORT2N uv, UBYTE4N weights, UBYTE4 indices - all mapped right; no DEC3N/UDEC3).
    - STRANGE: ANY patch of this shader (even a no-op FIND==REPLACE, from a copied patch dir) makes the characters
      solid BLACK, while the unpatched shader gives the foil. Next: compare the unpatched vs patched compile path
      (ShaderWarm cache reuse by source key? HDR/soft-shadow variants applied after patching?), and whether the
      shipped foil comes from an old cached library. Forcing vFace +1 or -1 also gave black (same patch effect).
    - Seen once: with RECOMP_D3D9_PASS_LOG listing 15 frames the render thread called a garbage address from the
      command ring (return 006a2cff) at 30 s - a race the long logging exposed; not seen otherwise.
    - Plate "holes": not found yet (the chase-camera plate is hidden behind the location HUD text in tests).
    - Continued (same night): the "any patch makes them black" runs were a MISREADING - those arrests happened backlit
      against the sky, and with the foil the shaded side goes pure black; a later exact-copy patch dir run had no
      arrest at all. Arrests are not deterministic (about every second run of the PQ recipe; Up 25 s, then stop).
    - KIT FIX kept (correct for ps_3_0, not the foil's cause): pixel shader float constants were 32 registers
      (ps_2_0's count) in D9Pipeline.pconst, SetPixelShaderConstantF, D3DX constant tables, the PS_CONST_SCALE /
      PS_SETTING_REG / recomp_d3d9_ps_reg_set paths and the probe; now 224. The character shader's c32..c41 are its
      own def constants, which reached it already.
    - Foil shape: lit side right, the shaded side cut to black with a hard edge - the shadow/light factor is binary
      there (select on r5.w / shcmp), so the next suspects are the character shadow pass (14b283be46760bd7 writes
      packed depth fract(z*c1)+c0 into the R32F 3072x1024 map, enlarged 2x by TARGET_SCALE "3072x1024:2") and the
      ambient terms (c20/c23/c11 ...). Next: RECOMP_D3D9_CONST_PROBE on an arrest frame with the shader's names, and
      a run with TARGET_SCALE without the shadow-map rule.
    - All my shadow changes off at once (shadow_reflection_detail=2 -> 1x maps, draw_distance=0 -> game cascades,
      shadow_softness=0): two arrests, the foil unchanged. So it is in the base port's rendering of the character
      shader, not in any enhancement. Next: CONST_PROBE of 2a6ebd55f9cba503's constants on an arrest frame, then show
      its intermediates (shadow factor, light terms) as colour with a patch (patching works; the "black" was backlight).
  * 2026-11-24 "FOIL" CLOTHES / BLACK EYES FIXED: the character shader (code 2a6ebd55f9cba503) produced NaN over large
    areas (x-ray: isnan of the light sum before the final curve). It takes log2 of saturated dots (0 -> -inf) and
    multiplies by c22.w = 0 (character_specular_intensity.w): 0 x -inf = NaN in IEEE/Metal, 0 on Direct3D 9 hardware
    (the "0 * anything = 0" rule DXVK emulates with d3d9.floatEmulation). mods/core/nfsu/shaders/2a6ebd55f9cba503.patch
    does that one product the D3D9 way. Checked on arrests: cops' blue shirts, badges, the driver's jacket, faces -
    no black patches. NOTE for other black-patch bugs: the same 0 x inf rule may bite other shaders; a kit-wide D3D9
    multiply emulation (mul/mad/dp with zero operands -> 0) would be the general fix (cost: a select per product).
    Earlier suspicions (AO_factors going negative, vFace, shadows, texture mods) were not the cause.
  * 2026-11-24 D3D9 FLOAT RULE KIT-WIDE: kit/dx/d3d9_shadergen.cpp emits MUL, MAD, DP3, DP4, DP2ADD in MSL through
    d9_mul/d9_dot* (0 x anything = 0, as D3D9 hardware / DXVK floatEmulation); RECOMP_D3D9_FLOAT_EMU=0 = plain IEEE.
    The character patch 2a6ebd55f9cba503.patch now finds nothing to patch (its FIND is the old plain product) and is
    kept harmless. Checked: arrests (cops fine), driving 116-118 fps unchanged, no compile failures.
    RECOMP_D3D9_NAN_MARK=1 (diagnostic): any pixel shader whose colour RGB is NaN draws red. With it the whole scene
    gets a faint uniform red tint (not HUD/menus): one background pass (likely the half/quarter-size blur or glow
    target, later blended in by the composite) still holds NaN, invisible normally (multiplied by 0 / blended away).
    Not identified yet - next: RECOMP_D3D9_SKIP_PS bisect among the post passes with the marker on. (Many world
    shaders also leave NaN in unused alpha - the marker checks RGB only.)
  * 2026-11-24 F10 PER SAVE + the person's defaults:
    - tools/setup_kit/defaults/mod-settings.json = the person's dist settings (62 keys), without retired keys and the
      money row (mod_eo_cash). A new profile starts from it.
    - Kit (generic): kernel32 create_file_named -> mods_settings_save_opened(<folder>) for any file opened under
      RECOMP_SAVES_DIR/<folder>/ (NFS Undercover opens Documents/NFS Undercover/<name>/<name> once at save load).
      settings.cpp keeps <profile>/SaveSettings/<name>.json with the save's own settings: all except apply="restart"
      (the launcher reads those before a save exists), host.* (window, frame limit, controls) and
      RECOMP_SETTINGS_GLOBAL list. A save without a file starts from RECOMP_SETTINGS_DEFAULTS (launch.c: the bundle's
      Defaults/mod-settings.json). Writes MERGE into the file (at shutdown the mods' settings are gone before the
      host's last save - a replace wrote an empty file). mod-settings.json still holds everything as last used.
      settings_menu.cpp: until a save is in use, F10 opens on the Saves page only ("Загрузите сохранение, чтобы
      открыть его настройки"); deleting a save also removes its settings file.
      Checked: before the save only Saves; "ru" loaded -> file made from the defaults; a prepared ru.json with bloom 2,
      redux 0, motion_blur 1 -> "its own settings (3 changed)" and the values in use.
    - NOT done: the 11 restart-only settings are global (shared by all saves), not applied at save load.
  * 2026-11-24 POLICE CAREER / ADD-ON CARS / PORSCHE PACK PER SAVE, LIVE (apply = "live" in the table now):
    - Police Career: the plugin is always loaded (NFSU_POLICE_CAREER=0 leaves it out). Its PoliceCareer.ini flags are
      copied at start into its object [0x101d69a0]+4..+0x1f (FUN_1016dbe0, copy at 0x1016d5d8: +9 PoliceRole, +0xa
      AmbientRacers ... +0x14 Interceptor), read while running (FUN_10134970 tests +9). nfsu.c pc_switch_pass: setting
      off -> +9..+0x1f = 0 and a police role in progress ends; on -> the plugin's own values. Off: no "Режим
      полицейского" in the pause menu; on: it is there (checked both on the person's save).
    - Add-on cars + Porsche Pack: launch.c reads their files ALWAYS (overlay rules with an empty setting: languages,
      GLOBALB_ALL.BUN, logos, vinyls) and the vault mounts always. addoncars.inc: groups 0/1 from "in addon/porsche"
      lines, g_addon_group_on read live (addon_groups_refresh); off: no car-lot rows added, addon_query_filter
      (AFTER FUN_00519850) drops entries of off cars from every car query (car lot, garages), no Russia section, the
      active-car guard gives the career a game car instead (also for a police car while Police Career is off).
      Checked: off -> shop Америка/Европа/Япония, no add-ons ("39 left out"); on -> Россия section back.
    - Not tested: switching in F10 in the middle of a drive (the passes re-read the settings every 200 ms).
  * 2026-11-24 F10 marks + HELICOPTER BOUGHT/SOLD:
    - F10 rows carry MenuLine.scope (1 shared, 2 this save's own; kit settings.cpp mods_settings_entry_per_save) drawn
      as a small tag after the label ("общая" grey / "сохранение" blue) and named in the footer timing line. Rows
      stay in their sections. Display/vsync/controls rows are shared.
    - Helicopter: heli_row_ensure's row (0x11f, slot 0xff, not owned) is now only the car lot's helicopter; FUN_0051df10
      ("on sale", thiscall row) answers yes for it (it said "not available"); CarSelected buys it the game's way
      (an owned row with copcar fe + SUV pv: seen 0x10b, flags 0x000f0002) unless one is owned already. heli_row_is =
      any such row (catalog or bought): flight, look, previews, garage name all follow it. No more garage injection
      (NFSU_HELI_GARAGE_ALWAYS=1 = old). Key 2 is blocked only on the patrol sedan; police_sale_remove removes a
      bought helicopter (never the catalog row). Checked in one run: bought -> active and flying, garage preview,
      sold -> "removed from the career: yes".
    - Left: the purchase dialog names it "Полицейский внедорожник"; the map icon (task 3) not done.
  * 2026-11-24 HELICOPTER PURCHASE DIALOG NAME: "Купить Полицейский внедорожник за 0 $?" came from the Police Career
    plugin's own name cache, not from its table: FUN_101348a0 (thiscall [0x101d69a0]+0x20, run once when the role
    object reaches state 5) looks each of its five cars' names up (table 0x101ae140, 20-byte rows {pv, preset, fe,
    text key, default}) and copies them in the game's text encoding into 128-byte buffers at +0xac + 0x80 x car.
    Pointing the table at other strings changed nothing. heli.inc heli_plugin_name (every main-loop pass): while a
    helicopter row is highlighted (g_fe_heli == 1, not driving) car 2's buffer (the SUV, the helicopter's pvehicle)
    holds "Полицейский вертолет", else the saved original (only saved if it reads as the SUV's name; rewritten if
    the plugin copies its names again). Seen on a frame dump: "Купить Полицейский вертолет за 0 $?".
    NFSU_HELI_NAMELOG=1 (state each second), NFSU_HELI_NAMESCAN=1 (finds the SUV's name in the plugin's memory),
    NFSU_HELI_SHOPLOG=1 now also logs each shop highlight (row, frontend key). Trap: pkgrun2.sh's SECONDS count
    from the launch; the police shop needs ~95 s with the person's start keys (80 ended before the highlight).
  * 2026-11-24 BACKGROUND NaN FOUND AND REMOVED: kit RECOMP_D3D9_NAN_MARK_BITS="mask:value" (hex; keeps the NaN marker
    only in pixel shaders whose code key & mask == value, + _NAN_MARK_LOG=1 names them) bisected the faint red tint to
    ONE shader, the 15-tap blur 61e40bbdebf3dade (756x491 quarter targets and the reflection mips). A patch showing
    which input was bad said: the constants. kit RECOMP_D3D9_CONST_NANLOG=<ps code key> (d3d9_metal.mm, draws whose
    c0-c15 hold NaN/inf) and RECOMP_D3D9_PS_NAN_CALLER=1 (d3d9.cpp SetPixelShaderConstantF, guest return address +
    stack) led to FUN_00769d60 (Gaussian taps {x, y, 0, weight}, stdcall size/out/deviation/multiplier/horizontal):
    FUN_00798200 passes deviation [0x00c21a80 + 4 x type] and the exe holds 0.0 for type 1 -> centre weight 0/0, all
    15 NaN after normalising, every frame - the game's own data (on Windows too; the 8-bit target stores 0). _CIexp
    and the translation are right (NFSU_GAUSS_LOG=1 logs inputs, weights and each exp). core.nfsu (always): an AFTER
    hook on 0x00769d60 sets NaN/inf weights to 0 -> the layer is 0 as before, no NaN (NFSU_GAUSS_NAN_KEEP=1 = old).
    Checked: NAN_MARK on every shader, red excess 60-65 -> ~5 (3 without the marker). Tools: /tmp/nanrun.sh TAG BITS,
    /tmp/nanbisect.sh (one run per key bit; the frame's mean R - (G+B)/2 > 20 = the tint is in that half).
  * 2026-11-24 F10 MOD SWITCHES MID-DRIVE, checked: driving on PQ's save, F10 -> Tab (Mods) -> 4 x Down = Police Career,
    +1 Add-on cars, +3 Porsche Pack (Right flips). On at 82-95 s, off at 112-125 s: after each menu close the log says
    "add-on cars for this career: add-ons on/off, Porsche Pack on/off" and "Police Career on/off" (role enabled 1/0);
    driving went on, no fault. The menu pauses the game, so a switch takes effect when it closes.
    TEST TRAP: `KS=...` on its own line is not exported - pkgrun2.sh then runs its default keys (export KS or put it
    before the command).
  * 2026-11-24 F10 REDESIGN (the person: categories/subcategories, marks under each row, "Saved"/"Waiting for restart",
    floating buttons, short hints, Russian/English):
    - kit/mods/settings_menu.cpp: kSubs = ordered (page, group, category, subcategory) rows; a setting's manifest group
      names its subcategory ("graphics.resolution", "extraoptions.police", "controls.pad"...); "#display" = the kit's
      window/frame-rate rows (+ group graphics.display), "#detail" starts with World filtering; catch-alls "graphics",
      "" and "controls". tab_of: "graphics*" page 0, "controls*" page 2 (prefix! "controls.pad" first fell to Mods).
      Headers carry level 0 (category) / 1 (subcategory). Per-section Restore rows and the Graphics "Defaults" section
      are gone: page buttons SRC_DEFAULTS (restore_tab: every default of the page drafted), SRC_APPLY "Save", SRC_CLOSE,
      SRC_DISCARD (only while drafts exist, last so the others keep their index) are MenuLine.footer items, drawn in the
      bar at the panel's bottom; Up/Down reach them after the last row.
    - Marks (MenuLine scope_text "Shared"/"Save file «name»", timing_kind 1 immediate / 2 restart / 3 on Save / 4 other,
      state_kind 1 "Saved" / 2 "Waiting for restart" / 3 "Not saved", state_alpha): "Saved" for 2.6 s after an
      immediate change (g_changed_at, note_changed in adjust / apply_draft / slider), eased in 0.22 s and out 0.4 s.
      Switches only for bool settings (a choice named "Off" kept its arrows wrongly before).
    - Hints: "|" = a line break in a mod manifest's help (the TOML reader takes no escapes); kit strings use "\n".
      tools/dev/nfsu_settings_table.py was regenerated with short hints and the new groups (hints in /tmp/f10/hints.py
      at the time). menu_lang.cpp: every new string has Russian; English when the game isn't Russian (checked with
      RECOMP_MENU_LANG=en).
    - Drawer (settings_menu_draw.cpp): dark tint over the glass, segmented page tabs (accent = selected), category
      headers, accent subcategory labels over cards of rows with hairlines, two-line rows with chips, value capsules,
      fixed hint card (i icon, title, lines) and button bar; the list fades under the card. round_rect has a fast path
      for filled shapes (the fades redraw ~20/s at 3024x1964). Diagnostics: RECOMP_MENU_FADE_LOG=1 (each redraw's
      "Saved" strength), RECOMP_MENU_REFRESH_LOG=1 (parked-menu refreshes per 2 s; ~55/s measured).
    - Performance overlay default off (tools/setup_kit/defaults + the person's profile); tests turn it on if needed.
  * 2026-11-24 EXTRA OPTIONS police/lights: "Police in the city" off was one message (FUN_0065c7c0 -> 0xca7dccd6 ->
    AICopManager iface +0x40 slot 1 = 0x00403590 EnableCops writes [mgr+0x200]); a new manager per scene/race
    (ctor FUN_0043d8e0) sets it back to 1 - now resent every second while off and driving. Emergency lights never
    switched: eo_player_car found nothing; now the IVehicle (vtable 0x00c19594) of [[0x00de93d4]], as Police Career
    passes it - the bits [iv+0x9c] & 0xf000 switch (log "lit before 0, after 1"), but a civilian car has no light model,
    so nothing shows (only police cars). The person's save "ru" has the Extra Options keys off (mod_eo_hotkeys 0).
  * 2026-11-24 JOB PROMPT d-pad instead of Tab (pics/30): NOT solved. With no pad listed by DirectInput, no SetupAPI
    device and XInput "not connected" (XInputGetState never even called) the icon above an event stayed the d-pad at
    the I-20 start (dense dumps: constant), with pad_layout 0 too. So it is not device presence: likely a stored game
    setting (ControlConfig controllerId, SQL strings 0xbe750c/0xbe75b8 in FUN_0052e440) or the profile. Kept: XInput
    reports user 0 only with a real pad (kit host_pad_connected; RECOMP_XINPUT_ALWAYS=1 old). Reverted: DirectInput
    listing / SetupAPI conditional on the pad and a WM_DEVICECHANGE post (untested with a real pad, no effect here;
    joy_listed() kept unused). The game handles WM_DEVICECHANGE at 0075d2f9 ([0x00d4b544] = 1).
  * 2026-11-25 JOB PROMPT SOLVED + RELEASE CHECK:
    - The d-pad over events instead of "Tab" was the High Contrast HUD mod: its HUDTEXRACE.BIN (the game's pack changed
      in place, same size) replaces 23 textures, among them HUD_RACE_NOW_PC (the PC key cap) with a console d-pad.
      package.py hud_keep_textures() puts that one texture (picture + info) back from the game's GLOBAL/HUDTEXRACE.BIN;
      the other 22 stay. Device presence was ruled out first: with no pad listed (RECOMP_DINPUT_PAD_REAL=1 experiment,
      kept env-only; joy_listed()), no SetupAPI device and XInput not connected the icon stayed the d-pad.
    - Release bench tools/dev/perf/release/rel.sh (copy of the person's profile, save "Player1", Extra Options keys on,
      per-save overrides, autopilot by key 7 at 45 s, NOAUTO=" " skips it, EVERY/AFTER dump spacing) + suite.sh /
      suite2.sh. pkgrun2.sh now opens Chrome at the end (the person's wish, 2026-11-25).
    - FOUND + FIXED: (1) the money row: a save switch brought the new save's stored row value in and it was taken for
      typing (Player1 5000 -> 11000): a new save (mods_settings_active_save via dlsym) or career object first adopts the
      game's money. (2) every mod on: a call to address 0 from 0x00661c71 at save load - FUN_00661bb0 (HUD message)
      reads [[0x00de99dc]] only when [0x00de99e4] is set, else calls through null; eo_message now returns when there
      is no player HUD. (3) autopilot + front end: the autopilot is ended when the game state leaves 6.
    - Results (dist app, 3024x1964, vsync 120): base 117-119 fps; allmods2 117, no fault, cops off = no pursuit in 2+
      min; alloff 113-115; gfx variants 118-119; race by Tab won on autopilot 118; arrests at heat 5 normal. Police
      menu "Гараж" does nothing on a save with no police car (buy in "Магазин" first) - plugin behaviour. Hard
      Drivers per-race values not matched for the free-roam race tried (97 general values applied).
    - The full description for a future builder project: dist/NFSU_Mac_Port_-_full_description.txt (structured part
      in Russian + this file, game.toml and launch.c as appendices).
