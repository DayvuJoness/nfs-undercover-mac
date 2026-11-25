//! The Setup window: 1. Your game, 2. Apple's Command Line Tools, 3. Options, then the build area.

use crate::logic::{self, PortConfig};
use eframe::egui::{self, Color32, CornerRadius, FontFamily, FontId, Pos2, RichText, Sense, Stroke, Vec2};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::time::{Duration, Instant};

pub const WINDOW_WIDTH: f32 = 680.0;
const PADDING: f32 = 20.0;
const SPACING: f32 = 14.0;
const LOG_HEIGHT: f32 = 150.0;
const SCREENSHOT_FRAMES: u64 = 120;

// macOS light appearance.
const WINDOW_BG: Color32 = Color32::from_rgb(236, 236, 236);
const GROUP_BG: Color32 = Color32::from_rgb(227, 227, 227);
const GROUP_STROKE: Color32 = Color32::from_rgb(214, 214, 214);
const TEXT: Color32 = Color32::from_rgb(29, 29, 31);
const SECONDARY: Color32 = Color32::from_rgb(128, 128, 133);
const GREEN: Color32 = Color32::from_rgb(40, 180, 65);
const ORANGE: Color32 = Color32::from_rgb(255, 149, 0);
const ACCENT: Color32 = Color32::from_rgb(0, 122, 255);
const DISABLED_ACCENT: Color32 = Color32::from_rgb(160, 196, 240);

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Phase {
    Idle,
    Checking,
    Building,
    Done,
    Failed,
    Cancelled,
}

#[derive(Clone, Copy, PartialEq)]
enum Icon {
    Check,
    Warning,
    Hourglass,
    Info,
}

enum Msg {
    Checked { ok: bool, text: String, cfg: PortConfig, icon: Icon },
    Started(u32),
    Line(String),
    Finished(i32),
    FailedToStart(String),
    ToolsReady,
}

pub struct SetupApp {
    game: Option<PathBuf>,
    game_status: String,
    game_icon: Icon,
    game_ok: bool,
    cfg: PortConfig,
    /// The built-in mods (names) and whether this copy of the app carries its port source and mods.
    mods: Vec<String>,
    has_payload: bool,
    show_read_me: bool,
    dest: PathBuf,
    tools: bool,
    installing_tools: bool,
    phase: Phase,
    step: usize,
    step_name: String,
    detail: String,
    progress: f64,
    lines: Vec<String>,
    started: Instant,
    message: String,
    pid: Option<u32>,
    cancelled: bool,
    last_poll: Instant,
    confirm_quit: bool,
    allow_close: bool,
    tx: Sender<Msg>,
    rx: Receiver<Msg>,
    // --screenshot
    screenshot: Option<PathBuf>,
    frames: u64,
    shot_requested: bool,
    sample: bool,
    last_height: f32,
}

impl SetupApp {
    pub fn new(cc: &eframe::CreationContext<'_>, screenshot: Option<PathBuf>, state: Option<String>) -> Self {
        install_fonts(&cc.egui_ctx);
        install_style(&cc.egui_ctx);
        let (tx, rx) = channel();
        let has_payload = logic::bundled_source().is_some() && logic::bundled_mods().is_some();
        let mut app = SetupApp {
            game: None,
            game_status: String::new(),
            game_icon: Icon::Info,
            game_ok: false,
            cfg: PortConfig::default(),
            mods: logic::bundled_mod_names(),
            has_payload,
            show_read_me: false,
            dest: logic::default_destination(),
            tools: logic::has_command_line_tools(),
            installing_tools: false,
            phase: Phase::Idle,
            step: 0,
            step_name: String::new(),
            detail: String::new(),
            progress: 0.0,
            lines: Vec::new(),
            started: Instant::now(),
            message: String::new(),
            pid: None,
            cancelled: false,
            last_poll: Instant::now(),
            confirm_quit: false,
            allow_close: false,
            tx,
            rx,
            screenshot,
            frames: 0,
            shot_requested: false,
            sample: false,
            last_height: 0.0,
        };
        if let Some(s) = state {
            app.apply_sample(&s);
        }
        app
    }

    /// The sample states the original's --snapshot drew (sample data; nothing runs).
    fn apply_sample(&mut self, state: &str) {
        self.sample = true;
        let sample_lines: Vec<String> = [
            "4/7 Analysing nfs.exe (once; can take a while)",
            "  downloading ghidra_12.1.3_PUBLIC_20260817.zip",
            "  unpacking ghidra_12.1.3_PUBLIC_20260817.zip",
            "  building Ghidra's decompiler for Apple Silicon (once)",
            "  $ .venv/bin/python tools/analyze.py --ghidra-home ...",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let ready = |s: &mut Self| {
            s.game = Some(PathBuf::from("/Users/you/Games/Need For Speed - Undercover"));
            s.game_ok = true;
            s.game_icon = Icon::Check;
            s.game_status = "Your game is ready to build.".into();
            s.tools = true;
            s.phase = Phase::Idle;
        };
        match state {
            "start" => {
                self.game = None;
                self.game_status.clear();
                self.tools = false;
            }
            "ready" => ready(self),
            "wrong-exe" => {
                ready(self);
                self.game_ok = false;
                self.game_icon = Icon::Warning;
                self.game_status = logic::wrong_exe_text(logic::DEFAULT_EXE);
            }
            "building" => {
                ready(self);
                self.phase = Phase::Building;
                self.step = 5;
                self.step_name = "Translating and building".into();
                self.detail = "Compiling: 812 of 2400".into();
                self.progress = logic::overall_progress(5, 0.34);
                self.lines = sample_lines;
                self.started = Instant::now() - Duration::from_secs(754);
            }
            "done" => {
                ready(self);
                self.phase = Phase::Done;
                self.progress = 1.0;
                self.message = format!("Built {} in 41 min 12 s.", logic::GAME_APP_NAME);
            }
            "failed" => {
                ready(self);
                self.phase = Phase::Failed;
                self.lines = sample_lines;
                self.lines.push(String::new());
                self.lines.push("SETUP STOPPED: step failed (exit 1); the last lines of build/setup.log say why".into());
                self.message = logic::failure_summary(&self.lines);
            }
            _ => self.sample = false,
        }
    }

    fn busy(&self) -> bool {
        matches!(self.phase, Phase::Checking | Phase::Building)
    }
    fn can_build(&self) -> bool {
        self.game_ok && self.has_payload && self.tools && !self.busy()
    }
    fn built_app(&self) -> PathBuf {
        self.dest.join(format!("{}.app", logic::GAME_APP_NAME))
    }

    // ------------------------------------------------------------ actions

    /// The person picks nfs.exe in the game folder they copied from their Windows PC.
    fn choose_game(&mut self) {
        if let Some(p) = rfd::FileDialog::new()
            .set_title(format!("Choose {} in your copied {} folder", self.cfg.executable, logic::GAME_TITLE))
            .add_filter("Windows program", &["exe", "EXE"])
            .pick_file()
        {
            self.use_game(p);
        }
    }

    /// nfs.exe chosen or dropped (its folder is the game), or the game folder itself dropped.
    fn use_game(&mut self, p: PathBuf) {
        if self.busy() {
            return;
        }
        self.game = Some(if p.is_file() { p.parent().map(Path::to_path_buf).unwrap_or(p) } else { p });
        self.check();
    }

    fn choose_dest(&mut self) {
        if let Some(p) = rfd::FileDialog::new().set_title("Where should the game app go?").set_directory(&self.dest).pick_folder() {
            self.dest = p;
        }
    }

    fn install_tools(&mut self) {
        let _ = logic::run_quick("/usr/bin/xcode-select", &["--install"]);
        self.installing_tools = true;
        let tx = self.tx.clone();
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_secs(5));
            if logic::has_command_line_tools() {
                let _ = tx.send(Msg::ToolsReady);
                break;
            }
        });
    }

    fn check(&mut self) {
        let Some(game) = self.game.clone() else { return };
        self.phase = Phase::Checking;
        self.game_ok = false;
        self.game_icon = Icon::Hourglass;
        self.game_status = "Checking… (the first time this also prepares the setup's own tools, about a minute)".into();
        let cfg = self.cfg.clone();
        let tx = self.tx.clone();
        std::thread::spawn(move || {
            let msg = |ok, text, cfg, icon| Msg::Checked { ok, text, cfg, icon };
            // The built-in port goes into the work folder first: its game.toml names the exact nfs.exe.
            if let Err(e) = logic::prepare_source() {
                let _ = tx.send(msg(false, e, cfg, Icon::Warning));
                return;
            }
            let cfg = logic::read_port_config(&logic::work_dir());
            let c = logic::check_game_folder(&game, &cfg);
            if !c.ok {
                let _ = tx.send(msg(false, c.text, cfg, Icon::Warning));
                return;
            }
            // The port's own check has the last word, as in the original.
            let (status, out) = logic::run_setup_collect(&logic::setup_arguments(&game, Path::new("/tmp"), true));
            let ok = status == 0 && out.iter().any(|l| l.contains("Check passed"));
            let text = if ok { "Your game is ready to build.".to_string() } else { logic::failure_summary(&out) };
            let _ = tx.send(msg(ok, text, cfg, if ok { Icon::Check } else { Icon::Warning }));
        });
    }

    fn build(&mut self) {
        let Some(game) = self.game.clone() else { return };
        if !self.can_build() {
            return;
        }
        self.phase = Phase::Building;
        self.cancelled = false;
        self.lines.clear();
        self.step = 0;
        self.step_name = "Starting".into();
        self.detail.clear();
        self.progress = 0.0;
        self.started = Instant::now();
        self.message.clear();
        let args = logic::setup_arguments(&game, &self.dest, false);
        let tx = self.tx.clone();
        std::thread::spawn(move || {
            if let Err(e) = logic::prepare_source() {
                let _ = tx.send(Msg::FailedToStart(e));
                return;
            }
            let mut child = match logic::setup_command(&args).spawn() {
                Ok(c) => c,
                Err(e) => {
                    let _ = tx.send(Msg::FailedToStart(e.to_string()));
                    return;
                }
            };
            let pid = child.id();
            let _ = tx.send(Msg::Started(pid));
            // Keep the Mac awake while the setup runs.
            let _ = std::process::Command::new("/usr/bin/caffeinate").args(["-i", "-w", &pid.to_string()]).spawn();
            if let Some(out) = child.stdout.take() {
                for l in BufReader::new(out).lines().map_while(Result::ok) {
                    let _ = tx.send(Msg::Line(l));
                }
            }
            let status = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
            let _ = tx.send(Msg::Finished(status));
        });
    }

    fn consume(&mut self, line: String) {
        if line.is_empty() {
            return;
        }
        if let Some(s) = logic::parse_step(&line) {
            self.step = s.number;
            self.step_name = s.name;
            self.detail.clear();
            self.progress = logic::overall_progress(self.step, 0.0);
        }
        self.lines.push(line);
        if self.lines.len() > logic::MAX_LOG_LINES {
            let extra = self.lines.len() - logic::MAX_LOG_LINES;
            self.lines.drain(..extra);
        }
    }

    fn poll(&mut self) {
        let (fraction, d) = logic::log_progress(&logic::read_log_tail(&logic::setup_log()));
        if let Some(d) = d {
            self.detail = d;
        }
        let within = if self.step == 5 { fraction.unwrap_or(0.0) } else { 0.0 };
        self.progress = logic::overall_progress(self.step, within);
    }

    fn cancel(&mut self) {
        if let Some(pid) = self.pid {
            self.cancelled = true;
            logic::terminate_tree(pid);
        }
    }

    fn finished(&mut self, status: i32) {
        self.pid = None;
        if self.cancelled {
            self.phase = Phase::Cancelled;
            self.message = "Stopped. Nothing in your game folder was changed; Build again to continue.".into();
        } else if status == 0 && self.built_app().exists() {
            self.phase = Phase::Done;
            self.progress = 1.0;
            self.message = format!("Built {} in {}.", logic::GAME_APP_NAME, self.elapsed());
        } else {
            self.phase = Phase::Failed;
            self.message = logic::failure_summary(&self.lines);
        }
    }

    fn elapsed(&self) -> String {
        logic::elapsed_text(self.started.elapsed().as_secs())
    }

    fn open_game(&self) {
        let _ = std::process::Command::new("/usr/bin/open").arg(self.built_app()).spawn();
    }
    fn reveal(path: &Path) {
        let _ = std::process::Command::new("/usr/bin/open").arg("-R").arg(path).spawn();
    }

    fn handle_messages(&mut self) {
        while let Ok(m) = self.rx.try_recv() {
            match m {
                Msg::Checked { ok, text, cfg, icon } => {
                    self.game_ok = ok;
                    self.game_status = text;
                    self.game_icon = icon;
                    self.cfg = cfg;
                    self.phase = Phase::Idle;
                }
                Msg::Started(pid) => self.pid = Some(pid),
                Msg::Line(l) => self.consume(l),
                Msg::Finished(s) => self.finished(s),
                Msg::FailedToStart(e) => {
                    self.phase = Phase::Failed;
                    self.message = e;
                }
                Msg::ToolsReady => {
                    self.tools = true;
                    self.installing_tools = false;
                }
            }
        }
    }

    fn handle_drops(&mut self, ctx: &egui::Context) {
        let dropped: Vec<PathBuf> = ctx.input(|i| i.raw.dropped_files.iter().filter_map(|f| f.path.clone()).collect());
        if let Some(p) = dropped.into_iter().next() {
            self.use_game(p);
        }
    }

    // ------------------------------------------------------------ drawing

    fn contents(&mut self, ui: &mut egui::Ui) {
        ui.spacing_mut().item_spacing = Vec2::new(8.0, SPACING);
        ui.horizontal(|ui| {
            ui.label(RichText::new(logic::GAME_APP_NAME).font(bold(26.0)).color(TEXT));
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if mac_button(ui, "Read Me", true, false).clicked() {
                    self.show_read_me = true;
                }
            });
        });
        ui.add(
            egui::Label::new(
                RichText::new(format!(
                    "Builds the native Mac version of {} from your own Steam copy of the PC game: the game bought on your Steam account and installed by Steam on a Windows PC. Everything happens on this Mac; nothing is uploaded, and your game folder is never changed.",
                    logic::GAME_TITLE
                ))
                .size(13.0)
                .color(SECONDARY),
            )
            .wrap(),
        );
        caption(ui, logic::CREDIT_LINE);

        let busy = self.busy();
        // 1. Your game
        group_box(ui, "1. Your game", |ui| {
            ui.spacing_mut().item_spacing.y = 6.0;
            ui.horizontal(|ui| {
                let (text, color) = match &self.game {
                    Some(g) => (g.display().to_string(), TEXT),
                    None => (
                        format!(
                            "Choose or drop {} from your own Steam copy of the game, copied from the Windows PC where Steam installed it (version {}).",
                            self.cfg.executable,
                            logic::GAME_VERSION
                        ),
                        SECONDARY,
                    ),
                };
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if mac_button(ui, "Choose…", !busy, false).clicked() {
                        self.choose_game();
                    }
                    ui.with_layout(egui::Layout::left_to_right(egui::Align::Center), |ui| {
                        ui.add(egui::Label::new(RichText::new(text).size(13.0).color(color)).wrap());
                    });
                });
            });
            if !self.game_status.is_empty() {
                let color = match self.game_icon {
                    Icon::Check => GREEN,
                    Icon::Warning => ORANGE,
                    Icon::Hourglass => SECONDARY,
                    Icon::Info => ACCENT,
                };
                icon_label(ui, self.game_icon, &self.game_status, color, 13.0);
            }
        });

        // 2. Apple's Command Line Tools
        group_box(ui, "2. Apple's Command Line Tools", |ui| {
            ui.horizontal(|ui| {
                if self.tools {
                    icon_label(ui, Icon::Check, "Installed.", GREEN, 13.0);
                } else if self.installing_tools {
                    icon_label(ui, Icon::Hourglass, "Finish Apple's installer window; this continues by itself.", TEXT, 13.0);
                } else {
                    ui.label(RichText::new("Needed to build the game (free, from Apple).").size(13.0).color(TEXT));
                }
                if !self.tools && !self.installing_tools {
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if mac_button(ui, "Install…", true, false).clicked() && !self.sample {
                            self.install_tools();
                        }
                    });
                }
            });
        });

        // 3. Options
        group_box(ui, "3. Options", |ui| {
            ui.spacing_mut().item_spacing.y = 6.0;
            ui.horizontal(|ui| {
                ui.label(RichText::new("Put the game in:").size(13.0).color(TEXT));
                ui.label(RichText::new(middle_truncate(&self.dest.display().to_string(), 56)).size(13.0).color(TEXT));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if mac_button(ui, "Change…", !busy, false).clicked() {
                        self.choose_dest();
                    }
                });
            });
            if self.has_payload {
                ui.label(RichText::new(format!("Built into the game: {} mods", self.mods.len())).size(13.0).color(TEXT));
                egui::CollapsingHeader::new(RichText::new("Mods and their authors").size(12.0).color(TEXT))
                    .id_salt("mods-authors")
                    .default_open(false)
                    .show(ui, |ui| {
                        ui.spacing_mut().item_spacing.y = 2.0;
                        for m in &self.mods {
                            caption(ui, m);
                        }
                    });
                caption(
                    ui,
                    "Built in as the tested Mac version has them; switch each in the game's F10 menu (Mods page). This Setup app carries the port's source code and these mods; the game itself is made from your nfs.exe.",
                );
            } else {
                icon_label(
                    ui,
                    Icon::Warning,
                    "This copy of the Setup app has no built-in port or mods. Download the Setup app again.",
                    ORANGE,
                    13.0,
                );
            }
        });

        self.build_area(ui);
    }

    fn build_area(&mut self, ui: &mut egui::Ui) {
        match self.phase {
            Phase::Building => {
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 8.0;
                    progress_bar(ui, self.progress as f32);
                    ui.horizontal(|ui| {
                        let t = if self.step > 0 {
                            format!("Step {} of {}: {}", self.step, logic::STEP_COUNT, self.step_name)
                        } else {
                            self.step_name.clone()
                        };
                        ui.label(RichText::new(t).font(bold(13.0)).color(TEXT));
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            ui.label(RichText::new(self.elapsed()).size(13.0).color(TEXT));
                        });
                    });
                    if !self.detail.is_empty() {
                        caption(ui, &self.detail);
                    }
                    caption(
                        ui,
                        &format!(
                            "The first build takes a while (the one-time analysis of {} is the long part); updates are much quicker. Keep the Mac plugged in; it stays awake while this runs.",
                            self.cfg.executable
                        ),
                    );
                    self.log_view(ui);
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if mac_button(ui, "Stop", true, false).clicked() {
                            self.cancel();
                        }
                    });
                });
            }
            Phase::Done => {
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 8.0;
                    icon_label_font(ui, Icon::Check, &self.message.clone(), GREEN, bold(13.0));
                    caption(
                        ui,
                        &format!(
                            "Your saves and settings will be in {}. The first launch takes a little longer while the Mac prepares the game's shaders.",
                            logic::SAVES_DIR.trim_end_matches('/')
                        ),
                    );
                    ui.horizontal(|ui| {
                        if mac_button(ui, "Open the game", true, true).clicked() {
                            self.open_game();
                        }
                        if mac_button(ui, "Show in Finder", true, false).clicked() {
                            Self::reveal(&self.built_app());
                        }
                    });
                });
            }
            Phase::Failed | Phase::Cancelled => {
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 8.0;
                    let head = if self.phase == Phase::Failed { "The build stopped." } else { "Stopped." };
                    icon_label_font(ui, Icon::Warning, head, ORANGE, bold(13.0));
                    ui.add(egui::Label::new(RichText::new(&self.message).size(12.0).color(TEXT)).wrap().selectable(true));
                    self.log_view(ui);
                    ui.horizontal(|ui| {
                        if mac_button(ui, "Show the log", true, false).clicked() {
                            Self::reveal(&logic::setup_log());
                        }
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            let can = self.can_build();
                            if mac_button(ui, "Build again", can, false).clicked() {
                                self.build();
                            }
                        });
                    });
                });
            }
            Phase::Idle | Phase::Checking => {
                ui.horizontal(|ui| {
                    if self.built_app().exists() {
                        caption(ui, &format!("{} is already in that folder; building again updates it.", logic::GAME_APP_NAME));
                    } else if self.game_ok && !self.has_payload {
                        caption(ui, "This copy of the Setup app has no built-in port: download it again.");
                    }
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let can = self.can_build();
                        if mac_button(ui, "Build", can, true).clicked() {
                            self.build();
                        }
                    });
                });
            }
        }
    }

    fn log_view(&self, ui: &mut egui::Ui) {
        egui::Frame::new()
            .fill(Color32::WHITE)
            .stroke(Stroke::new(1.0, Color32::from_rgb(200, 200, 200)))
            .corner_radius(CornerRadius::same(4))
            .inner_margin(6.0)
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                egui::ScrollArea::vertical().max_height(LOG_HEIGHT - 12.0).min_scrolled_height(LOG_HEIGHT - 12.0).stick_to_bottom(true).auto_shrink([false, false]).show(ui, |ui| {
                    ui.spacing_mut().item_spacing.y = 1.0;
                    for l in &self.lines {
                        ui.add(egui::Label::new(RichText::new(l).font(FontId::monospace(11.0)).color(TEXT)).extend());
                    }
                });
            });
    }

    fn read_me(&mut self, ctx: &egui::Context) {
        let r = egui::Modal::new(egui::Id::new("read-me")).show(ctx, |ui| {
            ui.set_width(720.0);
            egui::ScrollArea::vertical().max_height(440.0).auto_shrink([false, false]).show(ui, |ui| {
                ui.add(egui::Label::new(RichText::new(logic::read_me_text()).font(FontId::monospace(12.0)).color(TEXT)).selectable(true));
            });
            ui.add_space(12.0);
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if mac_button(ui, "Done", true, true).clicked() {
                    self.show_read_me = false;
                }
            });
        });
        if r.should_close() {
            self.show_read_me = false;
        }
    }

    fn quit_dialog(&mut self, ctx: &egui::Context) {
        egui::Modal::new(egui::Id::new("quit")).show(ctx, |ui| {
            ui.set_width(340.0);
            ui.label(RichText::new("Stop building the game?").font(bold(13.0)).color(TEXT));
            ui.add(
                egui::Label::new(
                    RichText::new("The build stops now. Building again later continues from the one-time analysis if it finished.").size(12.0).color(TEXT),
                )
                .wrap(),
            );
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                if mac_button(ui, "Keep Building", true, false).clicked() {
                    self.confirm_quit = false;
                }
                if mac_button(ui, "Stop and Quit", true, true).clicked() {
                    self.cancel();
                    self.allow_close = true;
                    self.confirm_quit = false;
                    ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
                }
            });
        });
    }

    fn screenshot_step(&mut self, ctx: &egui::Context) {
        let Some(path) = self.screenshot.clone() else { return };
        self.frames += 1;
        ctx.request_repaint();
        let shots: Vec<std::sync::Arc<egui::ColorImage>> = ctx.input(|i| {
            i.events
                .iter()
                .filter_map(|e| if let egui::Event::Screenshot { image, .. } = e { Some(image.clone()) } else { None })
                .collect()
        });
        if let Some(img) = shots.into_iter().next() {
            match save_png(&path, &img) {
                Ok(()) => println!("saved {}", path.display()),
                Err(e) => eprintln!("could not save {}: {e}", path.display()),
            }
            self.allow_close = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            return;
        }
        if self.frames >= SCREENSHOT_FRAMES && !self.shot_requested {
            self.shot_requested = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(Default::default()));
        }
    }
}

impl eframe::App for SetupApp {
    fn clear_color(&self, _: &egui::Visuals) -> [f32; 4] {
        egui::Rgba::from(WINDOW_BG).to_array()
    }

    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.handle_messages();
        if !self.sample {
            self.handle_drops(ctx);
        }
        if self.phase == Phase::Building {
            // build 0x10000bed8 polls the log every 2.0 s (0x10000cc50).
            if !self.sample && self.last_poll.elapsed() >= Duration::from_secs(2) {
                self.last_poll = Instant::now();
                self.poll();
            }
            ctx.request_repaint_after(Duration::from_millis(500));
        } else if self.busy() || self.installing_tools {
            ctx.request_repaint_after(Duration::from_millis(250));
        }
        if ctx.input(|i| i.viewport().close_requested()) && self.phase == Phase::Building && !self.allow_close && !self.sample {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.confirm_quit = true;
        }

        let hovering = ctx.input(|i| !i.raw.hovered_files.is_empty());
        let mut height = 0.0;
        egui::CentralPanel::default()
            .frame(egui::Frame::new().fill(WINDOW_BG).inner_margin(PADDING))
            .show(ctx, |ui| {
                let r = ui.vertical(|ui| self.contents(ui)).response.rect;
                height = r.height() + 2.0 * PADDING;
                if hovering {
                    let r = ui.max_rect().expand(PADDING - 4.0);
                    ui.painter().rect_stroke(r, CornerRadius::same(8), Stroke::new(3.0, ACCENT), egui::StrokeKind::Inside);
                }
            });
        if self.show_read_me {
            self.read_me(ctx);
        }
        if self.confirm_quit {
            self.quit_dialog(ctx);
        }

        // Size the window to its contents (the original's window follows its content size).
        let want = if self.show_read_me { height.max(600.0) } else { height };
        if (want - self.last_height).abs() > 1.0 {
            self.last_height = want;
            let w = if self.show_read_me { WINDOW_WIDTH.max(780.0) } else { WINDOW_WIDTH };
            ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(Vec2::new(w, want.ceil())));
        }
        self.screenshot_step(ctx);
    }
}

// ---------------------------------------------------------------- widgets

fn bold(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("bold".into()))
}

fn caption(ui: &mut egui::Ui, text: &str) {
    ui.add(egui::Label::new(RichText::new(text).size(11.0).color(SECONDARY)).wrap());
}

fn group_box(ui: &mut egui::Ui, title: &str, add: impl FnOnce(&mut egui::Ui)) {
    ui.vertical(|ui| {
        ui.spacing_mut().item_spacing.y = 5.0;
        ui.label(RichText::new(title).font(bold(13.0)).color(TEXT));
        egui::Frame::new()
            .fill(GROUP_BG)
            .stroke(Stroke::new(1.0, GROUP_STROKE))
            .corner_radius(CornerRadius::same(6))
            .inner_margin(egui::Margin::same(10))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                add(ui);
            });
    });
}

/// A macOS push button: white and bordered, or blue when prominent.
fn mac_button(ui: &mut egui::Ui, text: &str, enabled: bool, prominent: bool) -> egui::Response {
    let (fill, color, stroke) = match (prominent, enabled) {
        (true, true) => (ACCENT, Color32::WHITE, Stroke::NONE),
        (true, false) => (DISABLED_ACCENT, Color32::from_rgb(240, 245, 252), Stroke::NONE),
        (false, true) => (Color32::WHITE, TEXT, Stroke::new(1.0, Color32::from_rgb(196, 196, 196))),
        (false, false) => (Color32::from_rgb(246, 246, 246), Color32::from_rgb(176, 176, 176), Stroke::new(1.0, Color32::from_rgb(214, 214, 214))),
    };
    let b = egui::Button::new(RichText::new(text).size(13.0).color(color))
        .fill(fill)
        .stroke(stroke)
        .corner_radius(CornerRadius::same(5))
        .min_size(Vec2::new(0.0, 22.0));
    ui.add_enabled(enabled, b)
}

fn progress_bar(ui: &mut egui::Ui, value: f32) {
    let w = ui.available_width();
    let (rect, _) = ui.allocate_exact_size(Vec2::new(w, 6.0), Sense::hover());
    let p = ui.painter();
    p.rect_filled(rect, CornerRadius::same(3), Color32::from_rgb(214, 214, 214));
    let mut fill = rect;
    fill.set_width(rect.width() * value.clamp(0.0, 1.0));
    p.rect_filled(fill, CornerRadius::same(3), ACCENT);
}

fn icon_label(ui: &mut egui::Ui, icon: Icon, text: &str, color: Color32, size: f32) {
    icon_label_font(ui, icon, text, color, FontId::proportional(size));
}

fn icon_label_font(ui: &mut egui::Ui, icon: Icon, text: &str, color: Color32, font: FontId) {
    ui.horizontal_top(|ui| {
        ui.spacing_mut().item_spacing.x = 6.0;
        let (rect, _) = ui.allocate_exact_size(Vec2::splat(font.size + 3.0), Sense::hover());
        paint_icon(ui.painter(), rect.shrink(1.0), icon, color);
        ui.add(egui::Label::new(RichText::new(text).font(font).color(color)).wrap());
    });
}

fn paint_icon(p: &egui::Painter, r: egui::Rect, icon: Icon, color: Color32) {
    let c = r.center();
    let rad = r.width() / 2.0;
    match icon {
        Icon::Check => {
            p.circle_filled(c, rad, color);
            let pts = [Pos2::new(c.x - rad * 0.45, c.y + rad * 0.02), Pos2::new(c.x - rad * 0.1, c.y + rad * 0.38), Pos2::new(c.x + rad * 0.48, c.y - rad * 0.35)];
            p.line_segment([pts[0], pts[1]], Stroke::new(rad * 0.28, Color32::WHITE));
            p.line_segment([pts[1], pts[2]], Stroke::new(rad * 0.28, Color32::WHITE));
        }
        Icon::Info => {
            p.circle_filled(c, rad, color);
            p.circle_filled(Pos2::new(c.x, c.y - rad * 0.45), rad * 0.15, Color32::WHITE);
            p.line_segment([Pos2::new(c.x, c.y - rad * 0.1), Pos2::new(c.x, c.y + rad * 0.55)], Stroke::new(rad * 0.25, Color32::WHITE));
        }
        Icon::Warning => {
            let pts = vec![Pos2::new(c.x, r.top()), Pos2::new(r.right(), r.bottom() - 1.0), Pos2::new(r.left(), r.bottom() - 1.0)];
            p.add(egui::Shape::convex_polygon(pts, color, Stroke::NONE));
            p.line_segment([Pos2::new(c.x, c.y - rad * 0.25), Pos2::new(c.x, c.y + rad * 0.35)], Stroke::new(rad * 0.22, Color32::WHITE));
            p.circle_filled(Pos2::new(c.x, c.y + rad * 0.62), rad * 0.12, Color32::WHITE);
        }
        Icon::Hourglass => {
            let s = Stroke::new(1.4, color);
            let (l, rr, t, b) = (r.left() + rad * 0.3, r.right() - rad * 0.3, r.top() + 1.0, r.bottom() - 1.0);
            p.line_segment([Pos2::new(l, t), Pos2::new(rr, t)], s);
            p.line_segment([Pos2::new(l, b), Pos2::new(rr, b)], s);
            p.line_segment([Pos2::new(l + 1.0, t), Pos2::new(rr - 1.0, b)], s);
            p.line_segment([Pos2::new(rr - 1.0, t), Pos2::new(l + 1.0, b)], s);
        }
    }
}

fn middle_truncate(s: &str, max: usize) -> String {
    let chars: Vec<char> = s.chars().collect();
    if chars.len() <= max {
        return s.to_string();
    }
    let keep = (max - 1) / 2;
    let head: String = chars[..keep].iter().collect();
    let tail: String = chars[chars.len() - (max - 1 - keep)..].iter().collect();
    format!("{head}…{tail}")
}

// ---------------------------------------------------------------- look

fn install_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let load = |path: &str| std::fs::read(path).ok();
    let regular = load("/System/Library/Fonts/SFNS.ttf").or_else(|| load("/System/Library/Fonts/Supplemental/Arial.ttf"));
    let bold_font = load("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
    let mono = load("/System/Library/Fonts/SFNSMono.ttf").or_else(|| load("/System/Library/Fonts/Menlo.ttc"));
    if let Some(d) = regular {
        fonts.font_data.insert("ui".into(), std::sync::Arc::new(egui::FontData::from_owned(d)));
        fonts.families.entry(FontFamily::Proportional).or_default().insert(0, "ui".into());
    }
    if let Some(d) = mono {
        fonts.font_data.insert("mono".into(), std::sync::Arc::new(egui::FontData::from_owned(d)));
        fonts.families.entry(FontFamily::Monospace).or_default().insert(0, "mono".into());
    }
    let mut bold_family: Vec<String> = Vec::new();
    if let Some(d) = bold_font {
        fonts.font_data.insert("bold".into(), std::sync::Arc::new(egui::FontData::from_owned(d)));
        bold_family.push("bold".into());
    }
    bold_family.extend(fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default());
    fonts.families.insert(FontFamily::Name("bold".into()), bold_family);
    ctx.set_fonts(fonts);
}

fn install_style(ctx: &egui::Context) {
    ctx.set_visuals(egui::Visuals::light());
    ctx.style_mut(|s| {
        s.visuals.panel_fill = WINDOW_BG;
        s.visuals.window_fill = WINDOW_BG;
        s.visuals.override_text_color = None;
        s.visuals.selection.bg_fill = ACCENT;
        s.visuals.widgets.inactive.bg_fill = Color32::WHITE;
        s.visuals.widgets.inactive.weak_bg_fill = Color32::WHITE;
        s.visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, Color32::from_rgb(180, 180, 180));
        s.visuals.widgets.hovered.bg_stroke = Stroke::new(1.0, Color32::from_rgb(150, 150, 150));
        s.visuals.widgets.active.bg_fill = ACCENT;
        s.visuals.window_corner_radius = CornerRadius::same(10);
        s.text_styles.insert(egui::TextStyle::Body, FontId::proportional(13.0));
        s.text_styles.insert(egui::TextStyle::Button, FontId::proportional(13.0));
        s.spacing.button_padding = Vec2::new(10.0, 3.0);
        s.spacing.icon_width = 14.0;
        s.spacing.icon_spacing = 6.0;
    });
}

fn save_png(path: &Path, img: &egui::ColorImage) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        if !dir.as_os_str().is_empty() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
    }
    let file = std::fs::File::create(path).map_err(|e| e.to_string())?;
    let [w, h] = img.size;
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w as u32, h as u32);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut wr = enc.write_header().map_err(|e| e.to_string())?;
    wr.write_image_data(img.as_raw()).map_err(|e| e.to_string())?;
    Ok(())
}
