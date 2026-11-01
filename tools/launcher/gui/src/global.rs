//! Global pages: iOS versions (downloaded / available), Pre-translate (the AOT image per iOS version), Settings and
//! host status.
use crate::theme::*;
use crate::App;
use eframe::egui;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use x86code_core::{aot, host, sim};

#[derive(Default)]
pub struct AotShared {
    status: Option<Result<aot::Status, String>>,
    busy: bool,
}

pub struct Aot {
    pub runtime_root: Option<PathBuf>,
    pub device: Option<PathBuf>,
    pub jc: PathBuf,
    pub paths: aot::Paths,
    pub cfg: aot::Config,
    pub shared: Arc<Mutex<AotShared>>,
    pub child: Option<Child>,
    pub progress: Option<aot::Progress>,
    pub last_poll: Instant,
    pub cores: usize,
    pub max_cores: usize,
    pub filter: String,
    pub only_attention: bool,
    pub message: String,
}

impl Aot {
    pub fn new(rt: Option<PathBuf>) -> Aot {
        let jc = rt.as_deref().map(sim::jc_for).unwrap_or_else(aot::Paths::default_jc);
        let paths = aot::Paths::new(&jc);
        let cfg = paths.load_config();
        let max_cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
        let a = Aot {
            runtime_root: rt,
            device: cfg.device.clone(),
            jc,
            paths,
            cfg,
            shared: Arc::new(Mutex::new(AotShared::default())),
            child: None,
            progress: None,
            last_poll: Instant::now() - Duration::from_secs(60),
            cores: (max_cores / 2).max(1),
            max_cores,
            filter: String::new(),
            only_attention: true,
            message: String::new(),
        };
        if a.runtime_root.is_some() {
            a.refresh();
        }
        a
    }

    pub fn select(&mut self, rt: PathBuf, device: Option<PathBuf>) {
        self.jc = sim::jc_for(&rt);
        self.paths = aot::Paths::new(&self.jc);
        self.cfg = self.paths.load_config();
        if device.is_some() {
            self.device = device;
        } else if self.cfg.device.as_ref().is_some_and(|d| sim::device_runtime(d) == rt) {
            self.device = self.cfg.device.clone();
        } else {
            self.device = sim::devices().into_iter().map(|d| d.1).find(|d| sim::device_runtime(d) == rt);
        }
        self.runtime_root = Some(rt);
        self.progress = None;
        self.refresh();
    }

    fn effective_cfg(&self) -> aot::Config {
        let mut c = self.cfg.clone();
        if let Some(r) = &self.runtime_root {
            c.runtime_root = r.clone();
        }
        c.device = self.device.clone();
        c
    }

    pub fn refresh(&self) {
        let sh = self.shared.clone();
        if sh.lock().unwrap().busy {
            return;
        }
        sh.lock().unwrap().busy = true;
        let (paths, cfg) = (aot::Paths::new(&self.jc), self.effective_cfg());
        std::thread::spawn(move || {
            let r = aot::plan(&paths, &cfg).map(|p| p.status(&paths)).map_err(|e| format!("{e:#}"));
            let mut g = sh.lock().unwrap();
            g.status = Some(r);
            g.busy = false;
        });
    }

    pub fn building(&self) -> bool {
        self.child.is_some()
    }

    pub fn tick(&mut self) {
        if let Some(c) = &mut self.child {
            if let Ok(Some(_)) = c.try_wait() {
                self.child = None;
                self.refresh();
            }
        }
        if self.last_poll.elapsed() > Duration::from_millis(500) && (self.child.is_some() || self.progress.is_none()) {
            self.last_poll = Instant::now();
            self.progress = std::fs::read(self.paths.progress()).ok().and_then(|b| serde_json::from_slice(&b).ok());
        }
    }

    fn start_build(&mut self, full: bool) {
        let _ = self.paths.save_config(&self.effective_cfg());
        let exe = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join("x86code-aot"))).unwrap_or_else(|| "x86code-aot".into());
        let mut cmd = Command::new(exe);
        cmd.arg("--jc").arg(&self.jc);
        if let Some(d) = &self.device {
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

    fn cancel(&mut self) {
        if let Some(c) = &self.child {
            unsafe {
                libc_kill(c.id() as i32, 15);
            }
        }
    }
}

extern "C" {
    #[link_name = "kill"]
    fn libc_kill(pid: i32, sig: i32) -> i32;
}

fn aot_alive(pid: u32) -> bool {
    pid != 0 && std::path::Path::new(&format!("/proc/{pid}")).exists()
}

impl App {
    // ---------------- iOS versions ----------------
    pub fn ios_page(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            page_header(ui, "iOS versions", "Each phone runs one iOS version. Download more here.");
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Min), |ui| {
                ui.add_space(4.0);
                if ui.add(secondary("↻  Check Apple")).clicked() {
                    let c = self.catalogue.clone();
                    std::thread::spawn(move || {
                        let v = host::catalogue(true);
                        *c.lock().unwrap() = Some(v);
                    });
                }
            });
        });
        let cat = self.catalogue.lock().unwrap().clone();
        let runtimes = self.runtimes.clone();
        let insts = self.insts.clone();
        let phones_on = move |dir: &str| insts.iter().filter(|i| i.runtime_dir == dir).count();
        ui.label(egui::RichText::new("On this computer").size(16.0).strong());
        ui.add_space(4.0);
        if runtimes.is_empty() {
            card(ui, |ui| ui.label(muted("No iOS version downloaded yet.")));
        }
        for r in &runtimes {
            let fetching = self.fetching.contains_key(&r.build);
            card(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 2.0;
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(format!("iOS {}", r.version)).size(17.0).strong());
                            ui.label(muted(format!("build {}", r.build)).size(12.0));
                            if fetching {
                                pill(ui, "Downloading…", ORANGE);
                            } else if r.present {
                                pill(ui, "Ready", GREEN);
                            } else {
                                pill(ui, "Not downloaded", RED);
                            }
                        });
                        let n = phones_on(&r.dir);
                        let size = self.sizes.lock().unwrap().get(&r.jc).copied();
                        let mut line = format!("{} phone{} use it", n, if n == 1 { "" } else { "s" });
                        if let Some(b) = size {
                            line += &format!(" · translated code cache {:.1} GB", b as f64 / 1e9);
                        }
                        ui.label(muted(line).size(12.5));
                        if fetching {
                            let tail = std::fs::read_to_string(host::fetch_log(&r.build)).ok().and_then(|s| s.lines().filter(|l| !l.trim().is_empty()).last().map(|l| l.to_string())).unwrap_or_default();
                            ui.label(muted(truncate(&tail, 120)).size(11.5).monospace());
                            ui.ctx().request_repaint_after(Duration::from_secs(2));
                        } else if !r.present {
                            ui.label(egui::RichText::new("The files are missing. Download it again to boot phones on this version.").color(ORANGE).size(12.5));
                        }
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if r.present {
                            if ui.add(secondary("Pre-translate")).clicked() {
                                self.aot.select(r.root.clone(), None);
                                self.view = crate::View::Aot;
                            }
                        } else if !fetching && ui.add(primary("⬇  Download")).clicked() {
                            match host::fetch(&r.build) {
                                Ok(pid) => {
                                    self.fetching.insert(r.build.clone(), pid);
                                    self.say(format!("Downloading iOS {} (several GB)", r.version));
                                }
                                Err(e) => self.say(format!("{e:#}")),
                            }
                        }
                    });
                });
            });
            ui.add_space(8.0);
        }
        ui.add_space(10.0);
        ui.label(egui::RichText::new("Available from Apple").size(16.0).strong());
        ui.add_space(4.0);
        match cat {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(muted("Asking Apple…"));
                });
            }
            Some(v) => {
                let v: Vec<host::Catalogue> = v.into_iter().filter(|c| !runtimes.iter().any(|r| r.build == c.build)).collect();
                if v.is_empty() {
                    ui.label(muted("Everything in Apple's list is here already.").size(12.5));
                }
                for c in v {
                    let fetching = self.fetching.contains_key(&c.build);
                    card(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.vertical(|ui| {
                                ui.spacing_mut().item_spacing.y = 2.0;
                                ui.horizontal(|ui| {
                                    ui.label(egui::RichText::new(if c.version.is_empty() { c.name.clone() } else { format!("iOS {}", c.version) }).size(16.0).strong());
                                    ui.label(muted(format!("build {}", c.build)).size(12.0));
                                    if fetching {
                                        pill(ui, "Downloading…", ORANGE);
                                    }
                                });
                                ui.label(muted(format!("{} · {:.1} GB download", c.name, c.gb)).size(12.5));
                                if fetching {
                                    let tail = std::fs::read_to_string(host::fetch_log(&c.build)).ok().and_then(|s| s.lines().filter(|l| !l.trim().is_empty()).last().map(|l| l.to_string())).unwrap_or_default();
                                    ui.label(muted(truncate(&tail, 120)).size(11.5).monospace());
                                    ui.ctx().request_repaint_after(Duration::from_secs(2));
                                }
                            });
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                if !fetching && ui.add(primary("⬇  Download")).clicked() {
                                    match host::fetch(&c.build) {
                                        Ok(pid) => {
                                            self.fetching.insert(c.build.clone(), pid);
                                            self.say(format!("Downloading iOS {} ({:.1} GB)", c.version, c.gb));
                                        }
                                        Err(e) => self.say(format!("{e:#}")),
                                    }
                                }
                            });
                        });
                    });
                    ui.add_space(8.0);
                }
            }
        }
    }

    // ---------------- Pre-translate ----------------
    pub fn aot_page(&mut self, ui: &mut egui::Ui) {
        page_header(ui, "Pre-translate", "Get apps ready ahead of time so they open fast. One cache per iOS version.");
        let present: Vec<host::Runtime> = self.runtimes.iter().filter(|r| r.present).cloned().collect();
        if present.is_empty() {
            card(ui, |ui| ui.label(muted("Download an iOS version first.")));
            return;
        }
        ui.horizontal(|ui| {
            ui.label(muted("iOS version"));
            let labels: Vec<String> = present.iter().map(|r| format!("iOS {}", r.version)).collect();
            let sel = present.iter().position(|r| Some(&r.root) == self.aot.runtime_root.as_ref());
            if let Some(i) = segmented(ui, &labels.iter().map(|s| s.as_str()).collect::<Vec<_>>(), sel, !self.aot.building()) {
                self.aot.select(present[i].root.clone(), None);
            }
            if sel.is_none() {
                self.aot.select(present[0].root.clone(), None);
            }
            ui.add_space(10.0);
            ui.label(muted("Apps from"));
            let phones: Vec<(String, PathBuf)> = self.insts.iter().filter(|i| Some(&i.runtime_root) == self.aot.runtime_root.as_ref()).map(|i| (i.name.clone(), i.dir.clone())).collect();
            let cur = phones.iter().find(|p| Some(&p.1) == self.aot.device.as_ref()).map(|p| p.0.clone()).unwrap_or("system only".into());
            egui::ComboBox::from_id_salt("aotdev").width(170.0).selected_text(cur).show_ui(ui, |ui| {
                for (n, d) in &phones {
                    if ui.selectable_label(Some(d) == self.aot.device.as_ref(), n).clicked() {
                        self.aot.device = Some(d.clone());
                        self.aot.refresh();
                    }
                }
            }).response.on_hover_text("Which phone's apps and feature choices the image follows");
        });
        ui.add_space(10.0);
        let (status, busy) = {
            let g = self.aot.shared.lock().unwrap();
            (g.status.clone(), g.busy)
        };
        let building = self.aot.building();
        let st_ok = status.as_ref().and_then(|s| s.as_ref().ok()).cloned();
        let need = st_ok.as_ref().map(|s| s.stale + s.missing).unwrap_or(0);
        card(ui, |ui| {
            ui.horizontal(|ui| {
                if building {
                    if ui.add(danger("✖  Cancel")).clicked() {
                        self.aot.cancel();
                    }
                } else {
                    let resumable = self.aot.progress.as_ref().is_some_and(|p| p.state == "cancelled" || p.state == "error");
                    let label = if resumable && need > 0 {
                        "⏵  Continue"
                    } else if need > 0 && st_ok.as_ref().is_some_and(|s| s.up_to_date > 0) {
                        "⟳  Update"
                    } else if need == 0 && st_ok.is_some() {
                        "✔  Ready"
                    } else {
                        "⚡  Start"
                    };
                    if ui.add_enabled(need > 0 || st_ok.as_ref().is_some_and(|s| !s.image_current), primary(label)).clicked() {
                        self.aot.start_build(false);
                    }
                    if ui.add(secondary("Redo all")).on_hover_text("Start over from scratch").clicked() {
                        self.aot.start_build(true);
                    }
                }
                if !self.aot.message.is_empty() {
                    ui.label(egui::RichText::new(&self.aot.message).color(RED));
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if busy {
                        ui.spinner();
                    } else if ui.add(small("↻")).on_hover_text("Check again").clicked() {
                        self.aot.refresh();
                    }
                });
            });
            ui.add_space(10.0);
            egui::Grid::new("aotopts").num_columns(2).spacing([16.0, 10.0]).show(ui, |ui| {
                ui.label(muted("How much"));
                let scopes = [("hot", "Common"), ("loaded", "More"), ("all", "Everything")];
                let sel = scopes.iter().position(|s| self.aot.cfg.scope == s.0);
                if let Some(i) = segmented(ui, &scopes.map(|s| s.1), sel, !building) {
                    self.aot.cfg.scope = scopes[i].0.into();
                    let _ = self.aot.paths.save_config(&self.aot.effective_cfg());
                    self.aot.refresh();
                }
                ui.end_row();
                ui.label(muted("Speed"));
                ui.horizontal(|ui| {
                    ui.add_enabled(!building, egui::Slider::new(&mut self.aot.cores, 1..=self.aot.max_cores));
                    ui.label(muted("cores").size(12.5));
                });
                ui.end_row();
                ui.label(muted("Skip off features"));
                ui.horizontal(|ui| {
                    let mut f = self.aot.cfg.follow_features;
                    if ui.add_enabled_ui(!building, |ui| toggle(ui, &mut f)).inner.changed() {
                        self.aot.cfg.follow_features = f;
                        let _ = self.aot.paths.save_config(&self.aot.effective_cfg());
                        self.aot.refresh();
                    }
                    ui.label(muted("leave out what the phone has turned off").size(12.5));
                });
                ui.end_row();
            });
            if let Some(p) = &self.aot.progress {
                let live = building || p.state == "building" && aot_alive(p.pid);
                if live || matches!(p.state.as_str(), "done" | "cancelled" | "error") {
                    ui.add_space(12.0);
                    hline(ui);
                    ui.add_space(8.0);
                    let frac = if p.chunks_total > 0 { p.chunks_done as f32 / p.chunks_total as f32 } else { 0.0 };
                    let col = match p.state.as_str() {
                        "done" => GREEN,
                        "error" => RED,
                        "cancelled" => ORANGE,
                        _ => ACCENT,
                    };
                    ui.horizontal(|ui| {
                        ui.label(egui::RichText::new(format!("{:.0}%", frac * 100.0)).size(20.0).strong());
                        ui.label(muted(format!("{} of {} parts", p.chunks_done, p.chunks_total)));
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if live {
                                ui.label(muted(format!("{} left", p.eta_s.map(fmt_s).unwrap_or("–".into()))));
                            }
                        });
                    });
                    ui.add(egui::ProgressBar::new(frac).desired_height(10.0).fill(col).corner_radius(5).animate(live));
                    if live && !p.current.is_empty() {
                        ui.label(muted(format!("Working on {}", p.current.join(", "))).size(12.5));
                    }
                    if !live {
                        let t = match p.state.as_str() { "done" => "✔  Done", "cancelled" => "Stopped", _ => "Something went wrong" };
                        ui.label(egui::RichText::new(t).color(col)).on_hover_text(&p.message);
                    }
                }
            }
        });
        ui.add_space(12.0);
        match &status {
            None => {
                ui.label(muted("Checking…"));
            }
            Some(Err(e)) => {
                card(ui, |ui| ui.label(egui::RichText::new(e).color(RED)));
            }
            Some(Ok(s)) => {
                let (img, ic) = if s.image_current { ("Ready", GREEN) } else if s.image_present { ("Needs update", ORANGE) } else { ("Not started", RED) };
                stat_row(ui, vec![
                    ("STATUS", img.into(), ic),
                    ("READY", format!("{} / {}", s.up_to_date, s.total), TEXT),
                    ("TO UPDATE", need.to_string(), if need > 0 { ORANGE } else { TEXT }),
                    ("SIZE", format!("{:.0} MB", s.chunks.iter().map(|c| c.slot_mb).sum::<f64>()), TEXT),
                ]);
                if s.dropped > 0 {
                    ui.label(muted(format!("{} more get ready the first time they're used.", s.dropped)).size(12.5));
                }
                ui.add_space(8.0);
                card(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.add(egui::TextEdit::singleline(&mut self.aot.filter).hint_text("🔍  Search").desired_width(240.0));
                        ui.add_space(8.0);
                        toggle(ui, &mut self.aot.only_attention);
                        ui.label(muted("Only show what needs work"));
                    });
                    ui.add_space(6.0);
                    egui::Grid::new("chunks").striped(true).num_columns(4).spacing([16.0, 5.0]).min_col_width(60.0).show(ui, |ui| {
                        for h in ["STATUS", "NAME", "SIZE", "NOTE"] {
                            ui.label(muted(h).size(11.0).strong());
                        }
                        ui.end_row();
                        let mut shown = 0;
                        let f = self.aot.filter.to_lowercase();
                        for c in &s.chunks {
                            if self.aot.only_attention && c.state == "up-to-date" {
                                continue;
                            }
                            if !f.is_empty() && !c.name.to_lowercase().contains(&f) {
                                continue;
                            }
                            shown += 1;
                            if shown > 400 {
                                break;
                            }
                            let (t, col) = match c.state.as_str() {
                                "up-to-date" => ("Ready", GREEN),
                                "stale" => ("Old", ORANGE),
                                _ => ("Missing", RED),
                            };
                            pill(ui, t, col);
                            ui.label(c.name.rsplit('/').next().unwrap_or(&c.name)).on_hover_text(&c.name);
                            ui.label(muted(format!("{:.1} MB", c.slot_mb)));
                            ui.add(egui::Label::new(muted(c.reasons.join("; "))).truncate());
                            ui.end_row();
                        }
                        if shown == 0 {
                            ui.label(muted(if self.aot.only_attention { "All done" } else { "Nothing matches" }));
                            ui.end_row();
                        }
                    });
                });
            }
        }
    }

    // ---------------- Settings / host ----------------
    pub fn settings_page(&mut self, ui: &mut egui::Ui) {
        page_header(ui, "Settings", "");
        let mut save = false;
        card(ui, |ui| {
            ui.label(egui::RichText::new("Hub").size(16.0).strong());
            ui.add_space(6.0);
            egui::Grid::new("hub").num_columns(2).spacing([16.0, 12.0]).show(ui, |ui| {
                ui.label("Live previews on the Phones page");
                if toggle(ui, &mut self.hub.thumbnails).changed() {
                    save = true;
                }
                ui.end_row();
                ui.label("Open a phone when it starts");
                if toggle(ui, &mut self.hub.open_on_start).changed() {
                    save = true;
                }
                ui.end_row();
                ui.label("Card size");
                if ui.add(egui::Slider::new(&mut self.hub.card_w, 200.0..=360.0).show_value(false)).drag_stopped() {
                    save = true;
                }
                ui.end_row();
            });
        });
        if save {
            let _ = host::save_hub(&self.hub);
        }
        ui.add_space(12.0);
        let (nrun, mb) = self.running_summary();
        let gpu = host::mtlvk_pids().len();
        card(ui, |ui| {
            ui.label(egui::RichText::new("This computer").size(16.0).strong());
            ui.add_space(6.0);
            stat_row(ui, vec![
                ("PHONES RUNNING", nrun.to_string(), if nrun > 0 { GREEN } else { TEXT }),
                ("MEMORY USED", if nrun > 0 { fmt_mb(mb) } else { "–".into() }, TEXT),
                ("GPU HELPERS", gpu.to_string(), if gpu > 0 { GREEN } else { MUTED }),
                ("FREE DISK", format!("{:.0} GB", self.disk.free_gb), if self.disk.free_gb < 20.0 { ORANGE } else { TEXT }),
            ]);
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                ui.label(muted(format!("Caches and phone data live in {}", aot::scratch().display())).size(12.5));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.add_enabled(!self.measuring, small(if self.measuring { "Measuring…" } else { "Measure disk use" })).clicked() {
                        self.measuring = true;
                        let sizes = self.sizes.clone();
                        let mut dirs: Vec<PathBuf> = self.runtimes.iter().map(|r| r.jc.clone()).collect();
                        dirs.extend(self.insts.iter().map(|i| i.prefix.clone()));
                        dirs.extend(self.insts.iter().map(|i| i.dir.clone()));
                        let ctx = ui.ctx().clone();
                        std::thread::spawn(move || {
                            for d in dirs {
                                let s = host::dir_size(&d);
                                sizes.lock().unwrap().insert(d, s);
                                ctx.request_repaint();
                            }
                        });
                    }
                });
            });
            let sizes = self.sizes.lock().unwrap().clone();
            if !sizes.is_empty() {
                self.measuring = false;
                ui.add_space(6.0);
                egui::Grid::new("sizes").num_columns(2).spacing([16.0, 4.0]).striped(true).show(ui, |ui| {
                    let mut v: Vec<(&PathBuf, &u64)> = sizes.iter().collect();
                    v.sort_by(|a, b| b.1.cmp(a.1));
                    for (p, b) in v {
                        let name = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
                        let what = if name.starts_with("jc-") || name == "jitcache" { "translated code" } else if name.starts_with("a64prefix") { "phone system files" } else { "phone data" };
                        ui.label(egui::RichText::new(format!("{name}  ")).size(12.5)).on_hover_text(p.display().to_string());
                        ui.label(muted(format!("{:.1} GB · {what}", *b as f64 / 1e9)).size(12.5));
                        ui.end_row();
                    }
                });
            }
        });
        ui.add_space(12.0);
        card(ui, |ui| {
            ui.label(egui::RichText::new("Standalone window").size(16.0).strong());
            ui.label(muted("The separate viewer window (simview) still works. It follows the main phone's boot.").size(12.5));
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                if ui.add(secondary(if sim::simview_running() { "Window is open" } else { "Open window" })).clicked() {
                    match sim::show() {
                        Ok(()) => self.say("Window opened"),
                        Err(e) => self.say(format!("{e:#}")),
                    }
                }
            });
        });
    }
}
