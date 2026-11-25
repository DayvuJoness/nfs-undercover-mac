//! The setup's rules: step lines, progress, failure summaries, the game check, the port source and the
//! setup process. The values follow MECHANICS.md.

use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub const GAME_APP_NAME: &str = "Need for Speed Undercover";
pub const GAME_TITLE: &str = "Need for Speed: Undercover";
pub const DEFAULT_EXE: &str = "nfs.exe";
/// Who made this Setup app and with what (the person, 2026-11-25), shown in the window and the Read Me.
pub const DEVELOPER: &str = "DayvuJoness";
pub const CREDIT_LINE: &str = "By DayvuJoness, made with Claude and Craft Anything. Based on the Need for Speed Carbon Setup app by vasist15-source.";
/// The port's nfs.exe (its game.toml [game] sha256): the Steam 1.0.0.1, 10,584,064 bytes, no SecuROM.
pub const GAME_VERSION: &str = "1.0.0.1, Steam";
pub const DEFAULT_EXE_SHA256: &str = "d1ecded04cbba387c99299d2159b29b5c04b1d095293013caa4b404a43c7d2e9";
/// The installer's payload: the port's source (no game files) and the built-in mods with their SHA-256 list.
pub const SOURCE_ARCHIVE: &str = "source.tar.gz";
pub const MODS_FOLDER: &str = "mods_to_add";
pub const DEFAULT_REQUIRED_DIRS: [&str; 5] = ["CARS", "FRONTEND", "GLOBAL", "SOUND", "TRACKS"];
pub const SAVES_DIR: &str = "~/Library/Application Support/NFS Undercover/";
pub const SUPPORT_DIR_NAME: &str = "Library/NFSU-Native-Setup";
/// parseStep 0x100001acc: "<digit>/7 <name>".
pub const STEP_COUNT: usize = 7;
/// What an update keeps: the one-time analysis, the Python environment, the build cache, the game link.
pub const KEPT_ON_UPDATE: [&str; 6] = ["analysis", ".venv", "build", "original", "rebased", "dist"];
/// The share of the whole build each step usually takes: the static array at 0x100034cc8 (stepWeights).
pub const STEP_WEIGHTS: [f64; 8] = [0.0, 0.01, 0.01, 0.01, 0.50, 0.42, 0.01, 0.04];
/// consume 0x10000d3e0 keeps 0xbb8 lines.
pub const MAX_LOG_LINES: usize = 3000;
/// readLogTail default 0x100004d5c returns 0x10000.
pub const LOG_TAIL_BYTES: u64 = 65536;

pub fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/tmp"))
}
pub fn support_dir() -> PathBuf {
    home().join(SUPPORT_DIR_NAME)
}
pub fn work_dir() -> PathBuf {
    support_dir().join("source")
}
pub fn setup_log() -> PathBuf {
    work_dir().join("build/setup.log")
}

#[derive(Clone, Debug, PartialEq)]
pub struct StepInfo {
    pub number: usize,
    pub name: String,
}

/// Lines like "4/7 Analysing nfs.exe (once; can take a while)".
pub fn parse_step(line: &str) -> Option<StepInfo> {
    let t = line.trim();
    let mut chars = t.chars();
    let n = chars.next()?.to_digit(10)? as usize;
    let rest = chars.as_str();
    if t.chars().count() <= 4 || !rest.starts_with(&format!("/{STEP_COUNT} ")) {
        return None;
    }
    Some(StepInfo { number: n, name: rest[3..].to_string() })
}

pub fn overall_progress(step: usize, within: f64) -> f64 {
    if step < 1 {
        return 0.0;
    }
    let done: f64 = STEP_WEIGHTS.iter().take(step.min(STEP_COUNT)).sum();
    let w = if step <= STEP_COUNT { STEP_WEIGHTS[step] } else { 0.0 };
    (done + w * within.clamp(0.0, 1.0)).min(1.0)
}

/// What went wrong, in the setup's own words: its SETUP STOPPED line or its last lines.
pub fn failure_summary(lines: &[String]) -> String {
    if let Some(i) = lines.iter().rposition(|l| l.contains("SETUP STOPPED") || l.contains("can't be used")) {
        let text = lines[i..].iter().take(4).cloned().collect::<Vec<_>>().join("\n").replace("SETUP STOPPED: ", "");
        return if text.contains("setup.log") { text + "\nClick Show the log to see them." } else { text };
    }
    let start = lines.len().saturating_sub(6);
    lines[start..].join("\n")
}

/// The last progress the setup log shows: the compile's [done/total], or the analysis's exported functions.
pub fn log_progress(text: &str) -> (Option<f64>, Option<String>) {
    for line in text.lines().rev() {
        if line.starts_with('[') {
            if let Some(close) = line.find(']') {
                let inner = &line[1..close];
                let parts: Vec<&str> = inner.split('/').collect();
                if parts.len() == 2 {
                    if let (Ok(a), Ok(b)) = (parts[0].trim().parse::<f64>(), parts[1].trim().parse::<f64>()) {
                        if b > 0.0 {
                            return (Some(a / b), Some(format!("Compiling: {} of {}", a as i64, b as i64)));
                        }
                    }
                }
            }
        }
        if let Some(p) = line.find("Exported ") {
            if line.contains("functions") {
                return (None, Some(format!("Analysing: {}", line[p..].replace(';', ","))));
            }
        }
    }
    (None, None)
}

pub fn read_log_tail(path: &Path) -> String {
    let Ok(mut f) = fs::File::open(path) else { return String::new() };
    let size = f.seek(SeekFrom::End(0)).unwrap_or(0);
    let _ = f.seek(SeekFrom::Start(size.saturating_sub(LOG_TAIL_BYTES)));
    let mut buf = Vec::new();
    let _ = f.read_to_end(&mut buf);
    String::from_utf8_lossy(&buf).into_owned()
}

pub fn elapsed_text(secs: u64) -> String {
    if secs >= 3600 {
        format!("{} h {:02} min", secs / 3600, (secs % 3600) / 60)
    } else {
        format!("{} min {:02} s", secs / 60, secs % 60)
    }
}

/// The setup's environment: the system's tools first, and none of the developer test variables.
pub fn setup_environment() -> HashMap<String, String> {
    let mut env: HashMap<String, String> = std::env::vars()
        .filter(|(k, _)| {
            !k.starts_with("RECOMP_")
                && !k.starts_with("NFSU_")
                && !k.starts_with("NFSC_")
                && !k.starts_with("POPM_")
                && !k.starts_with("QA_")
                && !k.starts_with("PYTHON")
                && k != "VIRTUAL_ENV"
        })
        .collect();
    env.insert("PATH".into(), "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin".into());
    env
}

pub fn run_quick(exe: &str, args: &[&str]) -> (i32, String) {
    match Command::new(exe).args(args).stdin(Stdio::null()).output() {
        Ok(o) => {
            let mut s = String::from_utf8_lossy(&o.stdout).into_owned();
            s.push_str(&String::from_utf8_lossy(&o.stderr));
            (o.status.code().unwrap_or(-1), s)
        }
        Err(e) => (-1, e.to_string()),
    }
}

pub fn has_command_line_tools() -> bool {
    run_quick("/usr/bin/xcode-select", &["-p"]).0 == 0
}

pub fn default_destination() -> PathBuf {
    let apps = PathBuf::from("/Applications");
    let probe = apps.join(".nfsu-setup-write-test");
    if fs::write(&probe, b"").is_ok() {
        let _ = fs::remove_file(&probe);
        apps
    } else {
        home().join("Applications")
    }
}

pub fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = fs::File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// game/name without regard to case (Windows game folders mix cases).
pub fn find_ci(dir: &Path, name: &str) -> Option<PathBuf> {
    fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).find(|p| {
        p.file_name().map(|n| n.to_string_lossy().eq_ignore_ascii_case(name)).unwrap_or(false)
    })
}

// ---------------------------------------------------------------- the built-in port and mods

/// What the port's game.toml says about the game.
#[derive(Clone, Debug)]
pub struct PortConfig {
    pub executable: String,
    pub sha256: Option<String>,
    pub required_dirs: Vec<String>,
}

impl Default for PortConfig {
    fn default() -> Self {
        PortConfig {
            executable: DEFAULT_EXE.into(),
            sha256: Some(DEFAULT_EXE_SHA256.into()),
            required_dirs: DEFAULT_REQUIRED_DIRS.iter().map(|s| s.to_string()).collect(),
        }
    }
}

fn toml_string(line: &str, key: &str) -> Option<String> {
    let l = line.trim();
    let rest = l.strip_prefix(key)?.trim_start();
    let rest = rest.strip_prefix('=')?.trim();
    let q = rest.chars().next()?;
    if q != '"' && q != '\'' {
        return None;
    }
    let body = &rest[1..];
    Some(body[..body.find(q)?].to_string())
}

fn toml_list(line: &str, key: &str) -> Option<Vec<String>> {
    let l = line.trim();
    let rest = l.strip_prefix(key)?.trim_start().strip_prefix('=')?.trim();
    let body = rest.strip_prefix('[')?;
    let body = &body[..body.find(']')?];
    Some(body.split(',').map(|s| s.trim().trim_matches('"').trim_matches('\'').to_string()).filter(|s| !s.is_empty()).collect())
}

/// Reads game.toml's [game] executable/sha256 and [setup] required_dirs.
pub fn read_port_config(root: &Path) -> PortConfig {
    let mut cfg = PortConfig::default();
    if let Ok(text) = fs::read_to_string(root.join("game.toml")) {
        let mut section = String::new();
        for line in text.lines() {
            let t = line.trim();
            if t.starts_with('[') {
                section = t.trim_matches(|c| c == '[' || c == ']').to_string();
                continue;
            }
            match section.as_str() {
                "game" => {
                    if let Some(v) = toml_string(t, "executable") {
                        cfg.executable = v;
                    }
                    if let Some(v) = toml_string(t, "sha256") {
                        cfg.sha256 = Some(v.to_lowercase());
                    }
                }
                "setup" => {
                    if let Some(v) = toml_list(t, "required_dirs") {
                        cfg.required_dirs = v;
                    }
                }
                _ => {}
            }
        }
    }
    cfg
}

/// Where the installer's own payload is: Resources/ of the app bundle, a payload/ folder beside the program,
/// or (a developer build) the payload/ folder of the project it was built from.
pub fn payload_dir() -> Option<PathBuf> {
    let has = |d: &Path| d.join(SOURCE_ARCHIVE).is_file();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            for d in [dir.join("../Resources"), dir.join("payload")] {
                if has(&d) {
                    return Some(d);
                }
            }
        }
    }
    // A developer build also finds the project's payload/ (release builds don't carry the project's path).
    #[cfg(debug_assertions)]
    {
        let dev = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("payload");
        if has(&dev) {
            return Some(dev);
        }
    }
    None
}

pub fn bundled_source() -> Option<PathBuf> {
    payload_dir().map(|d| d.join(SOURCE_ARCHIVE))
}

/// The built-in mods: (folder, list of every file's SHA-256).
pub fn bundled_mods() -> Option<(PathBuf, PathBuf)> {
    let d = payload_dir()?;
    let (mods, list) = (d.join(MODS_FOLDER), d.join(format!("{MODS_FOLDER}.sha256")));
    (mods.is_dir() && list.is_file()).then_some((mods, list))
}

/// The built-in mods (top folders of the list) as "Name - author", sorted by name.
pub fn bundled_mod_names() -> Vec<String> {
    let Some((_, list)) = bundled_mods() else { return Vec::new() };
    let mut folders: Vec<String> = fs::read_to_string(list)
        .unwrap_or_default()
        .lines()
        .filter_map(|l| l.split_once("  ").map(|(_, rel)| rel.split('/').next().unwrap_or(rel).to_string()))
        .collect();
    folders.sort();
    folders.dedup();
    let mut names: Vec<String> = folders
        .iter()
        .map(|f| {
            let (name, author) = mod_info(f);
            format!("{name} - {}", author.unwrap_or(AUTHOR_UNKNOWN))
        })
        .collect();
    names.sort_by_key(|n| n.to_lowercase());
    names
}

pub const AUTHOR_UNKNOWN: &str = "author not named in the mod";

/// A mod folder's name as people know the mod, and its author (2026-11-25). Sources: the mod's own readme or
/// script ("By:", "Author:", "Mod made by", "Conversion :"), else its page on nfsmods.xyz ("<title> by <author>"),
/// matched by the upload code the site puts before the file name (b46f5a-, e0655a-) or by title; Redux, Photorealistic
/// and the Corolla as the person (DayvuJoness) named them, 2026-11-25. None = no author known.
pub fn mod_info(folder: &str) -> (String, Option<&'static str>) {
    const KNOWN: [(&str, &str, Option<&str>); 24] = [
        ("corollav1", "Toyota Corolla", Some("LegSolo")),
        ("HON_NSX_GT_05", "Honda NSX-R GT", Some("YochiThMaster333")),
        ("Prostreet Sky Mod", "ProStreet Sky", Some("LegSolo")),
        ("PoliceCareer-1.0.2", "Police Career 1.0.2", Some("kolomei")),
        ("SILVIALADA2101", "Lada 1200s", Some("YochiThMaster333")),
        ("FOCUSSTLADA2114", "Lada Samara", Some("YochiThMaster333")),
        ("SUBWRXSTI04", "Subaru WRX STI '04", Some("YochiThMaster333")),
        ("Lexus GS430 '04", "Lexus GS430 '04", Some("PedroBLR (conversion; model from Drift Spirits)")),
        ("Toyota Corolla Trueno '83", "Toyota Corolla Trueno '83", Some("PedroBLR (conversion; model from Drift Spirits)")),
        ("Miura P400 SV for NFS Undercover", "Lamborghini Miura P400 SV", Some("N4SPEED")),
        ("Need for Speed Undercover Redux", "Undercover Redux", Some("Miscellaneous")),
        ("Photorealistic Need for Speed Undercover", "Photorealistic (with Extra Options)", Some("Miscellaneous and ExOpts Team")),
        ("NFS Undercover Porsche Pack", "Porsche Pack", Some("Toshiya Joshima (God Arm)")),
        ("Undercover High Contrast HUD", "High Contrast HUD", Some("TheShveyn04")),
        ("Revamped Pursuits (UC)", "Revamped Pursuits", Some("Splash45")),
        ("Hard Drivers", "Hard Drivers", Some("Wurf")),
        ("Alfa Romeo Brera", "Alfa Romeo Brera", Some("Toshiya Joshima (God Arm)")),
        ("Aston Martin One-77", "Aston Martin One-77", Some("Toshiya Joshima (God Arm)")),
        ("Ferrari FXX-K Evoluzione Addon", "Ferrari FXX-K Evoluzione", Some("Toshiya Joshima (God Arm)")),
        ("Koenigsegg CCXR Special Edition", "Koenigsegg CCXR Special Edition", Some("Toshiya Joshima (God Arm)")),
        ("Koenigsegg Regera", "Koenigsegg Regera", Some("Toshiya Joshima (God Arm)")),
        ("Lamborghini Aventador SVJ", "Lamborghini Aventador SVJ", Some("Toshiya Joshima (God Arm)")),
        ("Marussia B2", "Marussia B2", Some("LegSolo")),
        ("Mercedes-AMG GT R Addon", "Mercedes-AMG GT R", Some("Toshiya Joshima (God Arm)")),
    ];
    if let Some((_, name, author)) = KNOWN.iter().find(|(key, _, _)| folder.ends_with(key)) {
        return (name.to_string(), *author);
    }
    (folder.trim_end_matches(" Addon").to_string(), None)
}

/// Puts the built-in port source in the work folder, keeping the analysis, environment and build cache of an
/// earlier run, and the built-in mods in its dist/mods_to_add (where the port's packager reads them).
/// Nothing is done for a part that is already there in this version.
pub fn prepare_source() -> Result<(), String> {
    let src = bundled_source().ok_or("This Setup app is incomplete (no source). Download it again.")?;
    let work = work_dir();
    let version = sha256_file(&src).map_err(|e| format!("Could not read the setup: {e}"))?;
    let marker = work.join(".source-version");
    let source_ready =
        fs::read_to_string(&marker).map(|s| s.trim() == version).unwrap_or(false) && work.join("setup.sh").is_file();
    if !source_ready {
        let tmp = support_dir().join("source.new");
        let _ = fs::remove_dir_all(&tmp);
        fs::create_dir_all(&tmp).map_err(|e| format!("Could not prepare the setup: {e}"))?;
        let (status, out) = run_quick("/usr/bin/tar", &["-xzf", &src.to_string_lossy(), "-C", &tmp.to_string_lossy()]);
        if status != 0 || !tmp.join("setup.sh").is_file() {
            let _ = fs::remove_dir_all(&tmp);
            return Err(format!("Could not unpack the setup: {}", out.trim()));
        }
        fs::create_dir_all(&work).map_err(|e| e.to_string())?;
        for e in fs::read_dir(&work).map_err(|e| e.to_string())?.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !KEPT_ON_UPDATE.contains(&name.as_str()) {
                let p = e.path();
                let r = if p.is_dir() && !p.is_symlink() { fs::remove_dir_all(&p) } else { fs::remove_file(&p) };
                r.map_err(|e| e.to_string())?;
            }
        }
        for e in fs::read_dir(&tmp).map_err(|e| e.to_string())?.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !KEPT_ON_UPDATE.contains(&name.as_str()) {
                fs::rename(e.path(), work.join(&name)).map_err(|e| e.to_string())?;
            }
        }
        let _ = fs::remove_dir_all(&tmp);
        fs::write(&marker, &version).map_err(|e| e.to_string())?;
    }
    prepare_mods(&work)?;
    for (from, to) in PAYLOAD_FILES {
        prepare_file(&work, from, to)?;
    }
    Ok(())
}

/// Files of the tested build the port's build reuses, so the result is the same as the tested app:
/// - pgo/game.profdata: the speed profile it was compiled with (how often each translated function and branch ran
///   in play; no game code); the port's cmake uses <work>/build/pgo/game.profdata whenever it is there;
/// - rebased/PoliceCareer.asi: the person's Police Career plugin as tools/police_career.py adapted it to this
///   nfs.exe (the mod author's code with its addresses rewritten). Carrying it means the retail SecuROM nfs.exe the
///   mod ships with (EA's program, used only for that matching) is not carried; setup.py reuses a plugin whose
///   SHA-256 is the one game.toml pins.
pub const PAYLOAD_FILES: [(&str, &str); 2] =
    [("pgo/game.profdata", "build/pgo/game.profdata"), ("rebased/PoliceCareer.asi", "rebased/PoliceCareer.asi")];

fn prepare_file(work: &Path, from: &str, to: &str) -> Result<(), String> {
    let Some(src) = payload_dir().map(|d| d.join(from)).filter(|p| p.is_file()) else { return Ok(()) };
    let dest = work.join(to);
    if sha256_file(&dest).ok() == sha256_file(&src).ok() {
        return Ok(());
    }
    fs::create_dir_all(dest.parent().unwrap()).map_err(|e| e.to_string())?;
    let _ = fs::remove_file(&dest);
    let (s, d) = (src.to_string_lossy().into_owned(), dest.to_string_lossy().into_owned());
    if run_quick("/bin/cp", &["-c", &s, &d]).0 != 0 {
        fs::copy(&src, &dest).map_err(|e| format!("Could not copy {from}: {e}"))?;
    }
    Ok(())
}

/// The built-in mods into <work>/dist/mods_to_add (+ .sha256), copied again only when their list changed.
fn prepare_mods(work: &Path) -> Result<(), String> {
    let (mods, list) = bundled_mods().ok_or("This Setup app is incomplete (no built-in mods). Download it again.")?;
    let dist = work.join("dist");
    let dest = dist.join(MODS_FOLDER);
    let dest_list = dist.join(format!("{MODS_FOLDER}.sha256"));
    let want = fs::read(&list).map_err(|e| e.to_string())?;
    if dest.is_dir() && fs::read(&dest_list).map(|have| have == want).unwrap_or(false) {
        return Ok(());
    }
    fs::create_dir_all(&dist).map_err(|e| e.to_string())?;
    let _ = fs::remove_file(&dest_list);
    if dest.exists() {
        fs::remove_dir_all(&dest).map_err(|e| format!("Could not replace the old mods: {e}"))?;
    }
    // A clone when the Setup app is on the same disk (no space used), else a copy.
    let (from, to) = (mods.to_string_lossy().into_owned(), dest.to_string_lossy().into_owned());
    if run_quick("/bin/cp", &["-cR", &from, &to]).0 != 0 {
        let _ = fs::remove_dir_all(&dest);
        let (status, out) = run_quick("/bin/cp", &["-R", &from, &to]);
        if status != 0 {
            return Err(format!("Could not copy the built-in mods: {}", out.trim()));
        }
    }
    fs::write(&dest_list, want).map_err(|e| e.to_string())
}

pub fn setup_arguments(game: &Path, dest: &Path, check_only: bool) -> Vec<String> {
    let mut a = vec![work_dir().join("setup.sh").to_string_lossy().into_owned(), game.to_string_lossy().into_owned()];
    if check_only {
        a.push("--check-only".into());
        return a;
    }
    a.push("--out".into());
    a.push(dest.to_string_lossy().into_owned());
    a
}

/// /bin/sh setup.sh ... with its output and errors on one pipe, in the work folder.
pub fn setup_command(args: &[String]) -> Command {
    let mut c = Command::new("/bin/sh");
    c.arg("-c").arg("exec /bin/sh \"$@\" 2>&1").arg("sh").args(args);
    c.current_dir(work_dir()).env_clear().envs(setup_environment()).stdin(Stdio::null()).stdout(Stdio::piped());
    c
}

/// Runs the setup to the end and returns its exit status and every line it printed.
pub fn run_setup_collect(args: &[String]) -> (i32, Vec<String>) {
    let mut child = match setup_command(args).spawn() {
        Ok(c) => c,
        Err(e) => return (-1, vec![e.to_string()]),
    };
    let mut lines = Vec::new();
    if let Some(out) = child.stdout.take() {
        for l in BufReader::new(out).lines().map_while(Result::ok) {
            lines.push(l);
        }
    }
    let status = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
    (status, lines)
}

/// Every process started under pid (the setup runs Python, Ghidra, CMake and the compiler).
pub fn descendants(pid: u32) -> Vec<u32> {
    let mut all = Vec::new();
    let mut frontier = vec![pid];
    while let Some(p) = frontier.pop() {
        let (_, out) = run_quick("/usr/bin/pgrep", &["-P", &p.to_string()]);
        let kids: Vec<u32> = out.lines().filter_map(|l| l.trim().parse().ok()).collect();
        all.extend(&kids);
        frontier.extend(kids);
    }
    all
}

pub fn terminate_tree(pid: u32) {
    for p in descendants(pid).into_iter().rev() {
        unsafe { libc::kill(p as i32, libc::SIGTERM) };
    }
    unsafe { libc::kill(pid as i32, libc::SIGTERM) };
}

// ---------------------------------------------------------------- the game check

pub struct GameCheck {
    pub ok: bool,
    pub text: String,
    pub sha256: Option<String>,
}

/// The check that needs no port: the folder, the program, its folders, and the pinned hash when known.
pub fn check_game_folder(game: &Path, cfg: &PortConfig) -> GameCheck {
    let fail = |t: String| GameCheck { ok: false, text: t, sha256: None };
    if !game.is_dir() {
        return fail(format!("{} is not a folder.", game.display()));
    }
    let exe_name = &cfg.executable;
    let Some(exe) = find_ci(game, exe_name) else {
        return fail(format!(
            "No {exe_name} in this folder. Choose the installed game folder (the one with {exe_name}, GLOBAL, TRACKS …)."
        ));
    };
    let mut head = [0u8; 2];
    if fs::File::open(&exe).and_then(|mut f| f.read_exact(&mut head)).is_err() || &head != b"MZ" {
        return fail(format!("This {exe_name} is not a Windows program. Nothing was changed."));
    }
    let dirs: Vec<String> = fs::read_dir(game)
        .map(|d| d.flatten().filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().to_uppercase()).collect())
        .unwrap_or_default();
    let missing: Vec<&String> = cfg.required_dirs.iter().filter(|d| !dirs.contains(&d.to_uppercase())).collect();
    if !missing.is_empty() {
        let m: Vec<&str> = missing.iter().map(|s| s.as_str()).collect();
        return fail(format!("The game folder is incomplete: {} missing.", m.join(", ")));
    }
    let digest = match sha256_file(&exe) {
        Ok(d) => d,
        Err(e) => return fail(format!("Could not read {exe_name}: {e}")),
    };
    if let Some(pinned) = &cfg.sha256 {
        if &digest != pinned {
            return GameCheck { ok: false, text: wrong_exe_text(exe_name), sha256: Some(digest) };
        }
    }
    GameCheck { ok: true, text: format!("{exe_name}: OK"), sha256: Some(digest) }
}

pub fn wrong_exe_text(exe: &str) -> String {
    format!(
        "This {exe} can't be used. The port is built for one exact {exe}: the one Steam installs for your own Steam copy of the game (version {GAME_VERSION}, without SecuROM). A disc copy's {exe} is a different, protected program. Nothing was changed."
    )
}

pub fn read_me_text() -> String {
    let mods = bundled_mod_names();
    let mod_lines = if mods.is_empty() { "    (none found in this copy of the Setup app)".to_string() } else {
        mods.iter().map(|m| format!("    - {m}")).collect::<Vec<_>>().join("\n")
    };
    format!(
        "{GAME_APP_NAME} - Setup
{CREDIT_LINE}

This app builds the native Mac version of {GAME_TITLE} from YOUR OWN personal Steam copy of the PC game.
It holds no game program: it carries the port's source code and the built-in mods, and builds the game on
your Mac from your {DEFAULT_EXE}. Everything happens on your Mac; nothing of your game is uploaded.

You need:
  - An Apple Silicon Mac (M1 or later).
  - Your own personal Steam copy of the game: Need for Speed: Undercover bought on your Steam account and
    installed by Steam on a Windows PC, its whole installed folder copied to this Mac (the one with {DEFAULT_EXE},
    GLOBAL, TRACKS, CARS and so on). Not just any copy: the port is built for the exact {DEFAULT_EXE} Steam
    installs (version {GAME_VERSION}, without SecuROM); a disc copy or someone else's files will not do.
  - About 15 GB free, an internet connection for the first build (it downloads its build tools: Python packages,
    Ghidra and a Java runtime), and time: the one-time analysis of {DEFAULT_EXE} is the long part.

Then:
  1. Choose {DEFAULT_EXE} in your copied game folder, or drop it (or the folder) on the window. The app checks it
     first and changes nothing if it can't be used.
  2. If asked, install Apple's Command Line Tools (free; the app waits for them).
  3. Click Build. When it finishes, click \"Open the game\".

Built into the game, with their authors - thank you to every one of them (switch the mods in the game's
F10 menu, Mods page):
{mod_lines}

Developer: {DEVELOPER}. Tools: Claude (Anthropic) and Craft Anything.
This Setup app is based on \"Need for Speed Carbon Setup\", the installer of the Need for Speed: Carbon Mac port by
vasist15-source (github.com/vasist15-source/nfs-carbon-mac).
The port itself is built on the Need for Speed: Carbon Mac port by vasist15-source and on recomp-kit by
veritr1x (both MIT); the mods' own credits also thank nfsu360 (NFS-VltEd, Car Toolkit), MaxHwoy (Binary) and
Oleg Melashenko (ZModeler).

Your saves and settings: {SAVES_DIR}
The setup's own files (safe to delete after building; an update is quicker with them): ~/{SUPPORT_DIR_NAME}/

The build runs in seven steps: checking the game folder, linking the game, preparing its runtime DLLs,
analysing {DEFAULT_EXE} once (and adapting the built-in Police Career to it), translating and building, checking
the built-in mods, and packaging. Your game folder is never written to.

This project is not affiliated with or endorsed by Electronic Arts.
"
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn mod_authors() {
        let names = bundled_mod_names();
        for n in &names {
            println!("{n}");
        }
        assert_eq!(names.len(), 24);
        let unknown = names.iter().filter(|n| n.ends_with(AUTHOR_UNKNOWN)).count();
        assert_eq!(unknown, 0, "every built-in mod names its author");
    }

    #[test]
    fn steps_and_progress() {
        let s = parse_step("4/7 Analysing nfs.exe (once; can take a while)").unwrap();
        assert_eq!(s.number, 4);
        assert_eq!(s.name, "Analysing nfs.exe (once; can take a while)");
        assert!(parse_step("  linked /x").is_none());
        assert!((overall_progress(5, 0.34) - (0.53 + 0.42 * 0.34)).abs() < 1e-9);
        assert_eq!(overall_progress(0, 1.0), 0.0);
        assert!((overall_progress(7, 1.0) - 1.0).abs() < 1e-9);
        let (f, d) = log_progress("x\n[812/2400] Building C object\n");
        assert!((f.unwrap() - 812.0 / 2400.0).abs() < 1e-9);
        assert_eq!(d.unwrap(), "Compiling: 812 of 2400");
        assert_eq!(elapsed_text(754), "12 min 34 s");
        assert_eq!(elapsed_text(3720), "1 h 02 min");
        let lines: Vec<String> = ["a", "", "SETUP STOPPED: step failed (exit 1); the last lines of build/setup.log say why"]
            .iter().map(|s| s.to_string()).collect();
        assert!(failure_summary(&lines).ends_with("Click Show the log to see them."));
    }
}
