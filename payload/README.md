# payload/

What the Setup app carries besides its program (copied into `Contents/Resources` by `make_setup_app.sh`):

- `source.tar.gz` — made by `build.rs` from `../port-undercover/` (not in git).
- `mods_to_add/` — the 24 built-in mods (not in git: one texture pack is over GitHub's 100 MB limit). Copy them
  from the released app: `cp -R "Need for Speed Undercover Setup.app/Contents/Resources/mods_to_add" payload/`.
- `mods_to_add.sha256` — every mod file's SHA-256 (made by `build.rs`; the port's setup checks it in step 6).
- `pgo/game.profdata` — the speed profile the tested build was compiled with (no game code).
- `rebased/PoliceCareer.asi` — the Police Career plugin (by kolomei) adapted to the Steam `nfs.exe`.
