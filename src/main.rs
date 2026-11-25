//! Need for Speed Undercover - Setup.
//!
//! Builds the native Mac version of Need for Speed: Undercover from the player's own copy of the PC game by
//! running the Undercover port it carries (its setup.sh, seven "n/7" steps) with the mods it carries built in,
//! the way the Carbon Setup app runs the Carbon port it carries.
//!
//!   nfsu-setup                              the window
//!   nfsu-setup --screenshot out.png [--state start|ready|wrong-exe|building|done|failed]
//!   nfsu-setup --selftest NFS_EXE_OR_GAME_FOLDER OUT_FOLDER [--verbose]

mod logic;
mod ui;

use eframe::egui;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if let Some(i) = args.iter().position(|a| a == "--selftest") {
        std::process::exit(selftest(&args[i + 1..]));
    }
    let screenshot = args.iter().position(|a| a == "--screenshot").and_then(|i| args.get(i + 1)).map(std::path::PathBuf::from);
    let state = args.iter().position(|a| a == "--state").and_then(|i| args.get(i + 1)).cloned();

    let mut viewport = egui::ViewportBuilder::default()
        .with_title(format!("{} Setup", logic::GAME_APP_NAME))
        .with_inner_size([ui::WINDOW_WIDTH, 620.0])
        .with_min_inner_size([ui::WINDOW_WIDTH, 300.0])
        .with_resizable(false)
        .with_maximize_button(false);
    if screenshot.is_some() {
        viewport = viewport.with_active(false);
        #[cfg(target_os = "windows")]
        {
            viewport = viewport.with_window_level(egui::WindowLevel::AlwaysOnBottom);
        }
    }
    let mut options = eframe::NativeOptions { viewport, ..Default::default() };
    #[cfg(target_os = "macos")]
    if screenshot.is_some() {
        options.event_loop_builder = Some(Box::new(|b| {
            use winit::platform::macos::EventLoopBuilderExtMacOS;
            b.with_activate_ignoring_other_apps(false);
        }));
    }
    let r = eframe::run_native(
        &format!("{} Setup", logic::GAME_APP_NAME),
        options,
        Box::new(move |cc| Ok(Box::new(ui::SetupApp::new(cc, screenshot, state)))),
    );
    if let Err(e) = r {
        eprintln!("could not open the window: {e}");
        std::process::exit(1);
    }
}

/// --selftest NFS_EXE_OR_GAME_FOLDER OUT: the window's source preparation, check and build, printed to stdout.
/// It also prints every step line and each line of the build's own output with --verbose.
fn selftest(args: &[String]) -> i32 {
    use std::io::{BufRead, BufReader};
    use std::path::PathBuf;
    let verbose = args.iter().any(|a| a == "--verbose");
    let args: Vec<&String> = args.iter().filter(|a| *a != "--verbose").collect();
    if args.len() < 2 {
        println!("usage: nfsu-setup --selftest NFS_EXE_OR_GAME_FOLDER OUT_FOLDER [--verbose]");
        return 2;
    }
    let picked = PathBuf::from(args[0]);
    let game = if picked.is_file() { picked.parent().map(|p| p.to_path_buf()).unwrap_or(picked) } else { picked };
    let dest = PathBuf::from(args[1]);
    if let Err(e) = logic::prepare_source() {
        println!("prepare failed: {e}");
        return 1;
    }
    println!(
        "selftest: source {} in {}; {} built-in mods; tools installed: {}",
        logic::bundled_source().map(|p| p.display().to_string()).unwrap_or_default(),
        logic::work_dir().display(),
        logic::bundled_mod_names().len(),
        logic::has_command_line_tools()
    );
    let cfg = logic::read_port_config(&logic::work_dir());
    let c = logic::check_game_folder(&game, &cfg);
    println!("selftest: game check: {} ({})", c.text, c.sha256.as_deref().unwrap_or("-"));
    if !c.ok {
        return 1;
    }
    for check_only in [true, false] {
        let mut child = match logic::setup_command(&logic::setup_arguments(&game, &dest, check_only)).spawn() {
            Ok(c) => c,
            Err(e) => {
                println!("selftest: could not start the setup: {e}");
                return 1;
            }
        };
        let mut lines = Vec::new();
        if let Some(out) = child.stdout.take() {
            for l in BufReader::new(out).lines().map_while(Result::ok) {
                if let Some(s) = logic::parse_step(&l) {
                    println!("selftest: step {} ({}) -> progress {:.2}", s.number, s.name, logic::overall_progress(s.number, 0.0));
                } else if verbose {
                    println!("  | {l}");
                }
                lines.push(l);
            }
        }
        let status = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
        println!("selftest: {} exit {status}", if check_only { "check" } else { "build" });
        if status != 0 {
            println!("selftest: failure summary:\n{}", logic::failure_summary(&lines));
            return 1;
        }
    }
    let app = dest.join(format!("{}.app", logic::GAME_APP_NAME));
    println!("selftest: app {} at {}", if app.exists() { "built" } else { "MISSING" }, app.display());
    if app.exists() { 0 } else { 1 }
}
