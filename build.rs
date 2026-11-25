//! Makes the installer's payload from the project's port-undercover/ (the Undercover port's source, no game files
//! and no build products) and payload/mods_to_add/ (the built-in mods):
//!   payload/source.tar.gz        the source the installer unpacks into ~/Library/NFSU-Native-Setup/source
//!   payload/mods_to_add.sha256   every mod file's SHA-256, checked by the port's setup (step 6) before packaging

use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;

fn files(dir: &Path, base: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    let mut entries: Vec<_> = rd.flatten().map(|e| e.path()).collect();
    entries.sort();
    for p in entries {
        let name = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if name == ".DS_Store" || name.starts_with("._") {
            continue;
        }
        if p.is_dir() {
            files(&p, base, out);
        } else if p.is_file() {
            out.push(p.strip_prefix(base).unwrap().to_path_buf());
        }
    }
}

fn sha256(p: &Path) -> String {
    let mut f = fs::File::open(p).expect("read a mod file");
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).expect("read a mod file");
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    h.finalize().iter().map(|b| format!("{b:02x}")).collect()
}

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let port = root.join("port-undercover");
    let payload = root.join("payload");
    let mods = payload.join("mods_to_add");
    println!("cargo:rerun-if-changed=port-undercover");
    println!("cargo:rerun-if-changed=payload/mods_to_add");
    println!("cargo:rerun-if-changed=build.rs");
    fs::create_dir_all(&payload).unwrap();

    if port.join("setup.sh").is_file() {
        let archive = payload.join("source.tar.gz");
        let tmp = payload.join("source.tar.gz.new");
        let status = Command::new("/usr/bin/tar")
            .env("COPYFILE_DISABLE", "1")
            .args(["--exclude", ".DS_Store", "--exclude", "__pycache__", "--exclude", "._*", "-czf"])
            .arg(&tmp)
            .arg("-C")
            .arg(&port)
            .arg(".")
            .status()
            .expect("run tar");
        assert!(status.success(), "tar of port-undercover failed");
        fs::rename(&tmp, &archive).unwrap();
    } else {
        println!("cargo:warning=port-undercover/setup.sh is missing: the installer has no port to build");
    }

    if mods.is_dir() {
        let mut list = Vec::new();
        files(&mods, &mods, &mut list);
        let text: String = list
            .iter()
            .map(|rel| format!("{}  {}\n", sha256(&mods.join(rel)), rel.to_string_lossy()))
            .collect();
        fs::write(payload.join("mods_to_add.sha256"), text).unwrap();
    } else {
        println!("cargo:warning=payload/mods_to_add is missing: the installer has no built-in mods");
    }
}
