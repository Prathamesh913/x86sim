//! x86Code launcher: sim controls, the pre-translated (AOT) image and per-device feature toggles.
//! The logic lives in x86code-core; builds run as `x86code-aot` child processes (progress in aot/progress.json), so
//! a build survives the window and the CLI shows the same state.
use eframe::egui;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use x86code_core::{aot, features, sim};

fn main() -> eframe::Result<()> {
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([1060.0, 820.0]).with_title("x86Code launcher"),
        ..Default::default()
    };
    eframe::run_native("x86Code launcher", opts, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}

#[derive(Default)]
struct Shared {
    sim: Option<sim::SimStatus>,
    status: Option<Result<aot::Status, String>>,
    status_busy: bool,
}

struct App {
    jc: PathBuf,
    paths: aot::Paths,
    cfg: aot::Config,
    runtimes: Vec<(String, PathBuf)>,
    devices: Vec<(String, PathBuf)>,
    runtime: usize,
    device: usize,
    cores: usize,
    max_cores: usize,
    shared: Arc<Mutex<Shared>>,
    child: Option<Child>,
    progress: Option<aot::Progress>,
    last_poll: Instant,
    message: String,
    filter: String,
    only_attention: bool,
    features_changed: bool,
}

impl App {
    fn new(_cc: &eframe::CreationContext) -> App {
        let jc = aot::Paths::default_jc();
        let paths = aot::Paths::new(&jc);
        let cfg = paths.load_config();
        let runtimes = sim::runtimes();
        let devices = sim::devices();
        let runtime = runtimes.iter().position(|r| r.1 == cfg.runtime_root).unwrap_or(0);
        let device = devices.iter().position(|d| Some(&d.1) == cfg.device.as_ref()).unwrap_or(0);
        let max_cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
        let app = App {
            jc,
            paths,
            cfg,
            runtimes,
            devices,
            runtime,
            device,
            cores: (max_cores / 2).max(1),
            max_cores,
            shared: Arc::new(Mutex::new(Shared::default())),
            child: None,
            progress: None,
            last_poll: Instant::now() - Duration::from_secs(60),
            message: String::new(),
            filter: String::new(),
            only_attention: true,
            features_changed: false,
        };
        // sim status every 3 s
        let sh = app.shared.clone();
        std::thread::spawn(move || loop {
            let s = sim::status(1000);
            sh.lock().unwrap().sim = Some(s);
            std::thread::sleep(Duration::from_secs(2));
        });
        app.refresh_status();
        app
    }

    fn device_dir(&self) -> Option<PathBuf> {
        self.devices.get(self.device).map(|d| d.1.clone())
    }

    fn effective_cfg(&self) -> aot::Config {
        let mut c = self.cfg.clone();
        if let Some(r) = self.runtimes.get(self.runtime) {
            c.runtime_root = r.1.clone();
        }
        c.device = self.device_dir();
        c
    }

    fn refresh_status(&self) {
        let sh = self.shared.clone();
        if sh.lock().unwrap().status_busy {
            return;
        }
        sh.lock().unwrap().status_busy = true;
        let (paths, cfg) = (aot::Paths::new(&self.jc), self.effective_cfg());
        std::thread::spawn(move || {
            let r = aot::plan(&paths, &cfg).map(|p| p.status(&paths)).map_err(|e| format!("{e:#}"));
            let mut g = sh.lock().unwrap();
            g.status = Some(r);
            g.status_busy = false;
        });
    }

    fn building(&mut self) -> bool {
        if let Some(c) = &mut self.child {
            if let Ok(Some(_)) = c.try_wait() {
                self.child = None;
                self.refresh_status();
            }
        }
        self.child.is_some()
    }

    fn start_build(&mut self, full: bool) {
        let _ = self.paths.save_config(&self.effective_cfg());
        let exe = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join("x86code-aot"))).unwrap_or_else(|| "x86code-aot".into());
        let mut cmd = Command::new(exe);
        cmd.arg("--jc").arg(&self.jc);
        if let Some(d) = self.device_dir() {
            cmd.arg("--device").arg(d);
        }
        cmd.arg(if full { "build" } else { "update" });
        if full {
            cmd.arg("--full");
        }
        cmd.args(["--workers", &self.cores.to_string()]);
        let log = std::fs::File::create(aot::scratch().join("launcher-aot.log")).ok();
        cmd.stdin(Stdio::null());
        match log {
            Some(l) => {
                cmd.stdout(l.try_clone().map(Stdio::from).unwrap_or(Stdio::null())).stderr(Stdio::from(l));
            }
            None => {
                cmd.stdout(Stdio::null()).stderr(Stdio::null());
            }
        }
        match cmd.spawn() {
            Ok(c) => {
                self.child = Some(c);
                self.message.clear();
            }
            Err(e) => self.message = format!("cannot start x86code-aot: {e}"),
        }
    }

    fn cancel_build(&mut self) {
        if let Some(c) = &self.child {
            unsafe {
                libc_kill(c.id() as i32, 15);
            }
        }
    }

    fn poll_progress(&mut self) {
        if self.last_poll.elapsed() < Duration::from_millis(500) {
            return;
        }
        self.last_poll = Instant::now();
        self.progress = std::fs::read(self.paths.progress()).ok().and_then(|b| serde_json::from_slice(&b).ok());
    }
}

extern "C" {
    #[link_name = "kill"]
    fn libc_kill(pid: i32, sig: i32) -> i32;
}

fn fmt_s(s: f64) -> String {
    if s >= 3600.0 {
        format!("{}h{:02}m", (s / 3600.0) as u64, (s as u64 % 3600) / 60)
    } else if s >= 60.0 {
        format!("{}m{:02}s", (s / 60.0) as u64, s as u64 % 60)
    } else {
        format!("{s:.0}s")
    }
}

const GREEN: egui::Color32 = egui::Color32::from_rgb(90, 180, 100);
const ORANGE: egui::Color32 = egui::Color32::from_rgb(230, 160, 60);
const RED: egui::Color32 = egui::Color32::from_rgb(220, 90, 80);

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        ctx.request_repaint_after(Duration::from_millis(500));
        let building = self.building();
        self.poll_progress();
        let (simst, status, status_busy) = {
            let g = self.shared.lock().unwrap();
            (g.sim.clone(), g.status.clone(), g.status_busy)
        };

        egui::SidePanel::left("left").resizable(false).exact_width(390.0).show(ctx, |ui| {
            ui.add_space(6.0);
            ui.heading("Simulator");
            ui.separator();
            egui::Grid::new("simgrid").num_columns(2).spacing([10.0, 6.0]).show(ui, |ui| {
                ui.label("Runtime");
                egui::ComboBox::from_id_salt("rt").width(250.0).selected_text(self.runtimes.get(self.runtime).map(|r| r.0.clone()).unwrap_or("none".into())).show_ui(ui, |ui| {
                    for (i, r) in self.runtimes.iter().enumerate() {
                        ui.selectable_value(&mut self.runtime, i, &r.0);
                    }
                });
                ui.end_row();
                ui.label("Device");
                egui::ComboBox::from_id_salt("dev").width(250.0).selected_text(self.devices.get(self.device).map(|d| d.0.clone()).unwrap_or("none".into())).show_ui(ui, |ui| {
                    for (i, d) in self.devices.iter().enumerate() {
                        ui.selectable_value(&mut self.device, i, &d.0);
                    }
                });
                ui.end_row();
            });
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                let dev = self.device_dir();
                let rt = self.runtimes.get(self.runtime).map(|r| r.1.clone());
                let booted = simst.as_ref().is_some_and(|s| s.booted);
                if ui.add_enabled(!booted && dev.is_some(), egui::Button::new("▶ Start")).clicked() {
                    if let (Some(d), Some(r)) = (dev.clone(), rt.clone()) {
                        self.message = match sim::start(&d, &r, &self.jc) {
                            Ok(p) => format!("booting (pid {p})"),
                            Err(e) => format!("{e:#}"),
                        };
                    }
                }
                if ui.add_enabled(booted, egui::Button::new("■ Stop")).clicked() {
                    if let Some(d) = dev.clone() {
                        let msg = Arc::new(Mutex::new(String::new()));
                        std::thread::spawn(move || {
                            let _ = sim::stop(&d);
                            drop(msg);
                        });
                        self.message = "stopping".into();
                    }
                }
                if ui.button("🖵 Show").clicked() {
                    self.message = match sim::show() {
                        Ok(()) => "simview on :0".into(),
                        Err(e) => format!("{e:#}"),
                    };
                }
            });
            ui.add_space(4.0);
            match &simst {
                None => {
                    ui.label("status: …");
                }
                Some(s) => {
                    egui::Grid::new("st").num_columns(2).spacing([10.0, 3.0]).show(ui, |ui| {
                        let yn = |b: bool| if b { egui::RichText::new("yes").color(GREEN) } else { egui::RichText::new("no").color(egui::Color32::GRAY) };
                        ui.label("Booted");
                        ui.label(yn(s.booted));
                        ui.end_row();
                        ui.label("Home screen");
                        ui.label(yn(s.home));
                        ui.end_row();
                        ui.label("Processes");
                        ui.label(s.procs.to_string());
                        ui.end_row();
                        ui.label("CPU");
                        ui.label(format!("{:.0}%", s.cpu_pct));
                        ui.end_row();
                        ui.label("PSS");
                        ui.label(format!("{:.0} MB (anon {:.0} MB)", s.pss_mb, s.pss_anon_mb));
                        ui.end_row();
                        ui.label("simview");
                        ui.label(yn(s.simview));
                        ui.end_row();
                    });
                }
            }
            if !self.message.is_empty() {
                ui.add_space(4.0);
                ui.label(egui::RichText::new(&self.message).italics());
            }

            ui.add_space(14.0);
            ui.heading("Features");
            ui.separator();
            if let Some(dev) = self.device_dir() {
                let prof = features::load(&dev);
                ui.horizontal(|ui| {
                    ui.label("Preset:");
                    let cur = features::preset_of(&prof);
                    for (name, label) in [("full", "Full (default)"), ("lean", "Lean"), ("minimal", "Minimal")] {
                        if ui.selectable_label(cur.as_deref() == Some(name), label).clicked() {
                            let _ = features::apply_preset(&dev, name);
                            self.features_changed = true;
                        }
                    }
                    if cur.is_none() {
                        ui.label(egui::RichText::new("custom").italics());
                    }
                });
                ui.add_space(2.0);
                let mut total_off = 0u64;
                for f in features::list(&dev) {
                    let mut on = f.enabled;
                    ui.horizontal(|ui| {
                        let r = ui.checkbox(&mut on, &f.title).on_hover_text(&f.desc);
                        if r.changed() {
                            let _ = features::set(&dev, &f.id, on);
                            self.features_changed = true;
                        }
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            ui.label(egui::RichText::new(format!("~{} MB {}", f.cost_mb, if f.when == "boot" { "every boot" } else { "when used" })).weak());
                        });
                    });
                    if !on && f.default {
                        total_off += f.cost_mb;
                    }
                }
                ui.add_space(4.0);
                if total_off > 0 {
                    ui.label(format!("Saves up to ~{total_off} MB versus the default set."));
                }
                ui.horizontal(|ui| {
                    let booted = simst.as_ref().is_some_and(|s| s.booted);
                    if ui.add_enabled(booted, egui::Button::new("⟳ Restart sim to apply")).clicked() {
                        if let Some(r) = self.runtimes.get(self.runtime).map(|r| r.1.clone()) {
                            let (d, jc) = (dev.clone(), self.jc.clone());
                            std::thread::spawn(move || {
                                let _ = sim::restart(&d, &r, &jc);
                            });
                            self.message = "restarting with the new features".into();
                            self.features_changed = false;
                        }
                    }
                    if self.features_changed {
                        ui.label(egui::RichText::new("changes apply at the next boot").color(ORANGE));
                    }
                });
                ui.label(egui::RichText::new("Off features skip their jobs; the image can skip their chunks too (follow features below).").weak().small());
            }
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(6.0);
            ui.heading("Pre-translated code (AOT image)");
            ui.separator();
            ui.label(
                "Pre-translating converts the runtime's arm64 libraries to x86 code ahead of time, so apps and the home screen \
                 start without translating. It is a longer one-time upfront cost; after that only incremental updates are \
                 needed (a runtime update, an app change or a translator codegen change rebuilds just the affected chunks).",
            );
            ui.add_space(6.0);
            let st_ok = status.as_ref().and_then(|s| s.as_ref().ok()).cloned();
            let need = st_ok.as_ref().map(|s| s.stale + s.missing).unwrap_or(0);
            ui.horizontal(|ui| {
                if building {
                    if ui.button("✖ Cancel").clicked() {
                        self.cancel_build();
                    }
                } else {
                    let resumable = self.progress.as_ref().is_some_and(|p| p.state == "cancelled" || p.state == "error");
                    let label = if resumable && need > 0 {
                        format!("⏵ Resume ({need} chunks left)")
                    } else if need > 0 && st_ok.as_ref().is_some_and(|s| s.up_to_date > 0) {
                        format!("⟳ Update ({need} of {} chunks)", st_ok.as_ref().map(|s| s.total).unwrap_or(0))
                    } else {
                        "⚙ Pre-translate".to_string()
                    };
                    if ui.add_enabled(need > 0 || st_ok.as_ref().is_some_and(|s| !s.image_current), egui::Button::new(egui::RichText::new(label).strong())).clicked() {
                        self.start_build(false);
                    }
                    if ui.button("Full rebuild").on_hover_text("relayout every chunk (compacts the slab and the constant pool)").clicked() {
                        self.start_build(true);
                    }
                }
                ui.add_space(12.0);
                ui.label("Cores:");
                ui.add_enabled(!building, egui::Slider::new(&mut self.cores, 1..=self.max_cores));
                ui.label(egui::RichText::new("low priority (nice 19, idle IO)").weak());
            });
            ui.horizontal(|ui| {
                ui.label("Scope:");
                for (s, l) in [("hot", "trained code only"), ("loaded", "libraries any process loads"), ("all", "everything")] {
                    if ui.add_enabled(!building, egui::Button::selectable(self.cfg.scope == s, l)).clicked() {
                        self.cfg.scope = s.into();
                        let _ = self.paths.save_config(&self.effective_cfg());
                        self.refresh_status();
                    }
                }
                if ui.add_enabled(!building, egui::Checkbox::new(&mut self.cfg.follow_features, "follow features")).changed() {
                    let _ = self.paths.save_config(&self.effective_cfg());
                    self.refresh_status();
                }
            });

            // progress
            if let Some(p) = &self.progress {
                let live = building || p.state == "building" && aot_alive(p.pid);
                if live || matches!(p.state.as_str(), "done" | "cancelled" | "error") {
                    ui.add_space(6.0);
                    let frac = if p.chunks_total > 0 { p.chunks_done as f32 / p.chunks_total as f32 } else { 0.0 };
                    ui.add(egui::ProgressBar::new(frac).show_percentage().animate(live));
                    egui::Grid::new("prog").num_columns(4).spacing([18.0, 3.0]).show(ui, |ui| {
                        ui.label("Chunks");
                        ui.label(format!("{} / {} ({} unchanged, skipped)", p.chunks_done, p.chunks_total, p.chunks_skipped));
                        ui.label("Blocks");
                        ui.label(format!("{}", p.blocks));
                        ui.end_row();
                        ui.label("Written");
                        ui.label(format!("{:.0} MB", p.mb_written));
                        ui.label("Elapsed / ETA");
                        ui.label(format!("{} / {}", fmt_s(p.elapsed_s), p.eta_s.map(fmt_s).unwrap_or("–".into())));
                        ui.end_row();
                    });
                    ui.label(format!("Now: {}", if p.current.is_empty() { "–".to_string() } else { p.current.join(", ") }));
                    let col = match p.state.as_str() {
                        "done" => GREEN,
                        "error" => RED,
                        "cancelled" => ORANGE,
                        _ => egui::Color32::GRAY,
                    };
                    ui.label(egui::RichText::new(format!("{}: {}", p.state, p.message)).color(col));
                }
            }

            ui.add_space(8.0);
            ui.separator();
            ui.horizontal(|ui| {
                ui.heading("Image status");
                if ui.add_enabled(!status_busy, egui::Button::new("↻")).on_hover_text("re-check").clicked() {
                    self.refresh_status();
                }
                if status_busy {
                    ui.spinner();
                }
            });
            match &status {
                None => {
                    ui.label("checking…");
                }
                Some(Err(e)) => {
                    ui.label(egui::RichText::new(e).color(RED));
                }
                Some(Ok(s)) => {
                    let img = if s.image_current { egui::RichText::new("current").color(GREEN) } else if s.image_present { egui::RichText::new("out of date").color(ORANGE) } else { egui::RichText::new("missing").color(RED) };
                    ui.horizontal(|ui| {
                        ui.label(format!("{} —", s.image));
                        ui.label(img);
                    });
                    ui.label(format!(
                        "{} chunks: {} up to date, {} stale, {} missing; {} libraries beyond the {} MB budget stay translated on demand. Training: {} profiles, {} hot blocks.",
                        s.total, s.up_to_date, s.stale, s.missing, s.dropped, s.budget_mb, s.training_files, s.hot_blocks
                    ));
                    if need > 0 && !building {
                        ui.add_space(4.0);
                        egui::Frame::new().fill(egui::Color32::from_rgb(70, 50, 20)).inner_margin(8.0).corner_radius(4.0).show(ui, |ui| {
                            ui.horizontal(|ui| {
                                ui.label(egui::RichText::new(format!("{need} of {} chunks need rebuilding", s.total)).color(ORANGE).strong());
                                if ui.button("Rebuild now").clicked() {
                                    self.start_build(false);
                                }
                            });
                        });
                    }
                    ui.add_space(4.0);
                    ui.horizontal(|ui| {
                        ui.label("Filter:");
                        ui.text_edit_singleline(&mut self.filter);
                        ui.checkbox(&mut self.only_attention, "only stale / missing");
                    });
                    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                        egui::Grid::new("chunks").striped(true).num_columns(5).spacing([12.0, 2.0]).show(ui, |ui| {
                            ui.strong("State");
                            ui.strong("Chunk");
                            ui.strong("Slot");
                            ui.strong("Blocks");
                            ui.strong("Why");
                            ui.end_row();
                            let mut shown = 0;
                            for c in &s.chunks {
                                if self.only_attention && c.state == "up-to-date" {
                                    continue;
                                }
                                if !self.filter.is_empty() && !c.name.to_lowercase().contains(&self.filter.to_lowercase()) {
                                    continue;
                                }
                                shown += 1;
                                if shown > 400 {
                                    break;
                                }
                                let (t, col) = match c.state.as_str() {
                                    "up-to-date" => ("up to date", GREEN),
                                    "stale" => ("stale", ORANGE),
                                    _ => ("missing", RED),
                                };
                                ui.label(egui::RichText::new(t).color(col));
                                ui.label(c.name.rsplit('/').next().unwrap_or(&c.name)).on_hover_text(&c.name);
                                ui.label(format!("{:.1} MB", c.slot_mb));
                                ui.label(if c.blocks > 0 { c.blocks.to_string() } else { format!("{} hot", c.hot_blocks) });
                                ui.label(c.reasons.join("; "));
                                ui.end_row();
                            }
                            if shown == 0 {
                                ui.label(egui::RichText::new("nothing to show").weak());
                                ui.end_row();
                            }
                        });
                    });
                }
            }
        });
    }
}

fn aot_alive(pid: u32) -> bool {
    pid != 0 && std::path::Path::new(&format!("/proc/{pid}")).exists()
}
