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
        let mut check = false;
        page_header(ui, "iOS versions", "Each phone runs one iOS version. Download more here.", |ui| {
            check = ui.add(Btn::secondary("Check for updates").icon(icon::ARROW_CLOCKWISE)).clicked();
        });
        if check {
            let c = self.catalogue.clone();
            *c.lock().unwrap() = None;
            std::thread::spawn(move || {
                let v = host::catalogue(true);
                *c.lock().unwrap() = Some(v);
            });
        }
        let cat = self.catalogue.lock().unwrap().clone();
        let runtimes = self.runtimes.clone();
        let insts = self.insts.clone();
        let phones_on = move |dir: &str| insts.iter().filter(|i| i.runtime_dir == dir).count();
        section(ui, "On this computer", |_| {});
        let mut fetch: Option<(String, String)> = None;
        let mut aot_rt = None;
        list_card(ui, |ui| {
            if runtimes.is_empty() {
                empty_state(ui, icon::STACK, "No iOS version yet", "Download one below to start a phone.", |_| {});
            }
            for (k, r) in runtimes.iter().enumerate() {
                if k > 0 {
                    hline(ui);
                }
                let fetching = self.fetching.contains_key(&r.build);
                let n = phones_on(&r.dir);
                let size = self.sizes.lock().unwrap().get(&r.jc).copied();
                let mut line = format!("Build {} · {} phone{}", r.build, n, if n == 1 { "" } else { "s" });
                if let Some(b) = size {
                    line += &format!(" · {:.1} GB translated code", b as f64 / 1e9);
                }
                let (st, col) = if fetching { ("Downloading", WARN) } else if r.present { ("Installed", GOOD) } else { ("Files missing", WARN) };
                list_row(ui, icon::STACK, &format!("iOS {}", r.version), &line, |ui| {
                    if r.present {
                        if ui.add(Btn::secondary("Pre-translate").icon(icon::LIGHTNING)).clicked() {
                            aot_rt = Some(r.root.clone());
                        }
                    } else if !fetching && ui.add(Btn::primary("Download").icon(icon::DOWNLOAD_SIMPLE)).clicked() {
                        fetch = Some((r.build.clone(), format!("iOS {}", r.version)));
                    }
                    ui.add_space(S2);
                    badge(ui, st, col);
                });
                if fetching {
                    fetch_tail(ui, &r.build);
                } else if !r.present {
                    egui::Frame::new().inner_margin(egui::Margin { left: 60, right: 16, top: 0, bottom: 12 }).show(ui, |ui| {
                        ui.label(muted("Its files are missing. Download it again to start phones on this version."));
                    });
                }
            }
        });
        ui.add_space(S5);
        section(ui, "Available from Apple", |_| {});
        list_card(ui, |ui| match &cat {
            None => {
                egui::Frame::new().inner_margin(16).show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label(sub("Asking Apple…"));
                    });
                });
            }
            Some(v) => {
                let v: Vec<&host::Catalogue> = v.iter().filter(|c| !runtimes.iter().any(|r| r.build == c.build)).collect();
                if v.is_empty() {
                    egui::Frame::new().inner_margin(16).show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(icon::CHECK_CIRCLE).size(16.0).color(GOOD));
                            ui.label(sub("You have everything Apple offers."));
                        });
                    });
                }
                for (k, c) in v.iter().enumerate() {
                    if k > 0 {
                        hline(ui);
                    }
                    let fetching = self.fetching.contains_key(&c.build);
                    let short = c.name.trim_end_matches(" Simulator Runtime").trim().to_string();
                    let t = if !short.is_empty() { short } else if c.version.is_empty() { c.build.clone() } else { format!("iOS {}", c.version) };
                    let detail = format!("Build {} · {:.1} GB download", c.build, c.gb);
                    list_row(ui, icon::CLOUD_ARROW_DOWN, &t, &detail, |ui| {
                        if fetching {
                            badge(ui, "Downloading", WARN);
                        } else if ui.add(Btn::secondary("Download").icon(icon::DOWNLOAD_SIMPLE)).clicked() {
                            fetch = Some((c.build.clone(), t.clone()));
                        }
                    });
                    if fetching {
                        fetch_tail(ui, &c.build);
                    }
                }
            }
        });
        if let Some((b, l)) = fetch {
            self.download(&b, &l);
        }
        if let Some(rt) = aot_rt {
            self.aot.select(rt, None);
            self.view = crate::View::Aot;
        }
        ui.add_space(S5);
    }

    // ---------------- Pre-translate ----------------
    pub fn aot_page(&mut self, ui: &mut egui::Ui) {
        page_header(ui, "Pre-translate", "Get apps ready ahead of time so they open fast. One cache per iOS version.", |_| {});
        let present: Vec<host::Runtime> = self.runtimes.iter().filter(|r| r.present).cloned().collect();
        if present.is_empty() {
            card(ui, |ui| empty_state(ui, icon::LIGHTNING, "Nothing to pre-translate yet", "Download an iOS version first.", |_| {}));
            return;
        }
        let building = self.aot.building();
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = S2;
            let labels: Vec<String> = present.iter().map(|r| format!("iOS {}", r.version)).collect();
            let sel = present.iter().position(|r| Some(&r.root) == self.aot.runtime_root.as_ref());
            if let Some(i) = segmented(ui, &labels.iter().map(|s| s.as_str()).collect::<Vec<_>>(), sel, !building) {
                self.aot.select(present[i].root.clone(), None);
            }
            if sel.is_none() {
                self.aot.select(present[0].root.clone(), None);
            }
            ui.add_space(S3);
            ui.label(sub("Apps from"));
            let phones: Vec<(String, PathBuf)> = self.insts.iter().filter(|i| Some(&i.runtime_root) == self.aot.runtime_root.as_ref()).map(|i| (i.name.clone(), i.dir.clone())).collect();
            let cur = phones.iter().find(|p| Some(&p.1) == self.aot.device.as_ref()).map(|p| p.0.clone()).unwrap_or("System only".into());
            egui::ComboBox::from_id_salt("aotdev").icon(caret)
                .width(180.0)
                .selected_text(body(cur))
                .show_ui(ui, |ui| {
                    for (n, d) in &phones {
                        if ui.selectable_label(Some(d) == self.aot.device.as_ref(), n).clicked() {
                            self.aot.device = Some(d.clone());
                            self.aot.refresh();
                        }
                    }
                })
                .response
                .on_hover_text("Which phone's apps and feature choices to follow");
        });
        ui.add_space(S4);
        let (status, busy) = {
            let g = self.aot.shared.lock().unwrap();
            (g.status.clone(), g.busy)
        };
        let st_ok = status.as_ref().and_then(|s| s.as_ref().ok()).cloned();
        let need = st_ok.as_ref().map(|s| s.stale + s.missing).unwrap_or(0);

        // ---- status + actions
        card(ui, |ui| {
            ui.horizontal(|ui| {
                let (head, col, line) = match (&st_ok, &status) {
                    (Some(s), _) if building => ("Pre-translating", ACCENT, format!("{} of {} ready", s.up_to_date, s.total)),
                    (Some(s), _) if s.image_current && need == 0 => ("Ready", GOOD, format!("All {} parts are ready", s.total)),
                    (Some(s), _) if s.image_present => ("Needs an update", WARN, format!("{need} of {} parts to redo", s.total)),
                    (Some(s), _) => ("Not started", INK_3, format!("{} parts to translate", s.total)),
                    (None, Some(Err(_))) => ("Couldn't check", CRIT, String::new()),
                    _ => ("Checking…", INK_3, String::new()),
                };
                let (r, _) = ui.allocate_exact_size(egui::vec2(36.0, 36.0), egui::Sense::hover());
                ui.painter().rect_filled(r, 8.0, RAISED);
                ui.painter().text(r.center(), egui::Align2::CENTER_CENTER, icon::LIGHTNING, font(18.0), if col == INK_3 { INK_2 } else { col });
                ui.add_space(4.0);
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 1.0;
                    ui.label(title(head));
                    if !line.is_empty() {
                        ui.label(muted(line));
                    }
                });
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = S2;
                    if busy {
                        ui.spinner();
                    } else if ui.add(Btn::tool(icon::ARROW_CLOCKWISE)).on_hover_text("Check again").clicked() {
                        self.aot.refresh();
                    }
                    if building {
                        if ui.add(Btn::secondary("Cancel").icon(icon::X)).clicked() {
                            self.aot.cancel();
                        }
                    } else {
                        if ui.add(Btn::secondary("Redo all")).on_hover_text("Start over from scratch").clicked() {
                            self.aot.start_build(true);
                        }
                        let resumable = self.aot.progress.as_ref().is_some_and(|p| p.state == "cancelled" || p.state == "error");
                        let (label, ic) = if resumable && need > 0 {
                            ("Continue", icon::PLAY)
                        } else if need > 0 && st_ok.as_ref().is_some_and(|s| s.up_to_date > 0) {
                            ("Update", icon::ARROW_CLOCKWISE)
                        } else if need == 0 && st_ok.is_some() {
                            ("Up to date", icon::CHECK)
                        } else {
                            ("Start", icon::LIGHTNING)
                        };
                        if ui.add_enabled(need > 0 || st_ok.as_ref().is_some_and(|s| !s.image_current), Btn::primary(label).icon(ic)).clicked() {
                            self.aot.start_build(false);
                        }
                    }
                });
            });
            if !self.aot.message.is_empty() {
                ui.add_space(S2);
                ui.label(egui::RichText::new(&self.aot.message).size(T_SMALL).color(CRIT_INK));
            }
            if let Some(p) = &self.aot.progress {
                let live = building || p.state == "building" && aot_alive(p.pid);
                if live || matches!(p.state.as_str(), "done" | "cancelled" | "error") {
                    ui.add_space(S4);
                    let frac = if p.chunks_total > 0 { p.chunks_done as f32 / p.chunks_total as f32 } else { 0.0 };
                    let col = match p.state.as_str() {
                        "done" => GOOD,
                        "error" => CRIT,
                        "cancelled" => WARN,
                        _ => ACCENT,
                    };
                    ui.horizontal(|ui| {
                        ui.label(egui::RichText::new(format!("{:.0}%", frac * 100.0)).size(T_TITLE).family(semibold()).color(INK));
                        ui.label(muted(format!("{} of {} parts", p.chunks_done, p.chunks_total)));
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if live {
                                ui.label(muted(format!("{} left", p.eta_s.map(fmt_s).unwrap_or("–".into()))));
                            } else {
                                let t = match p.state.as_str() { "done" => "Last run finished", "cancelled" => "Last run stopped", _ => "Last run failed" };
                                ui.label(muted(t)).on_hover_text(&p.message);
                            }
                        });
                    });
                    ui.add(egui::ProgressBar::new(frac).desired_height(6.0).fill(col).corner_radius(3).animate(false));
                    if live && !p.current.is_empty() {
                        ui.add(egui::Label::new(muted(format!("Working on {}", p.current.join(", ")))).truncate());
                    }
                }
            }
        });
        ui.add_space(S3);
        match &status {
            None => {}
            Some(Err(e)) => {
                card(ui, |ui| ui.label(egui::RichText::new(e).size(T_SMALL).color(CRIT_INK)));
            }
            Some(Ok(s)) => {
                stat_row(ui, CARD, &[
                    ("Ready", format!("{}", s.up_to_date), &format!("of {}", s.total)),
                    ("To update", need.to_string(), ""),
                    ("Size", format!("{:.0} MB", s.chunks.iter().map(|c| c.slot_mb).sum::<f64>()), ""),
                ]);
                if s.dropped > 0 {
                    ui.add_space(S1);
                    ui.label(muted(format!("{} more get ready the first time they're used.", s.dropped)));
                }
            }
        }
        ui.add_space(S5);

        // ---- options
        section(ui, "Options", |_| {});
        list_card(ui, |ui| {
            form_row(ui, "How much", "Common is quick; Everything covers every app and framework.", false, |ui| {
                let scopes = [("hot", "Common"), ("loaded", "More"), ("all", "Everything")];
                let sel = scopes.iter().position(|s| self.aot.cfg.scope == s.0);
                if let Some(i) = segmented(ui, &scopes.map(|s| s.1), sel, !building) {
                    self.aot.cfg.scope = scopes[i].0.into();
                    let _ = self.aot.paths.save_config(&self.aot.effective_cfg());
                    self.aot.refresh();
                }
            });
            form_row(ui, "Speed", "CPU cores to use while translating.", false, |ui| {
                ui.label(sub(format!("{} cores", self.aot.cores)));
                ui.add_enabled(!building, egui::Slider::new(&mut self.aot.cores, 1..=self.aot.max_cores).show_value(false));
            });
            form_row(ui, "Skip turned-off features", "Leave out what the phone has turned off.", true, |ui| {
                let mut f = self.aot.cfg.follow_features;
                if ui.add_enabled_ui(!building, |ui| toggle(ui, &mut f)).inner.changed() {
                    self.aot.cfg.follow_features = f;
                    let _ = self.aot.paths.save_config(&self.aot.effective_cfg());
                    self.aot.refresh();
                }
            });
        });
        ui.add_space(S5);

        // ---- parts
        let Some(Ok(s)) = &status else { return };
        section(ui, "Parts", |ui| {
            ui.label(sub("Only what needs work"));
            toggle(ui, &mut self.aot.only_attention);
            ui.add_space(S3);
            search(ui, &mut self.aot.filter, "Search", 220.0);
        });
        list_card(ui, |ui| {
            let w = ui.available_width();
            let (st_w, size_w) = (110.0, 80.0);
            let name_w = ((w - st_w - size_w - 32.0) * 0.5).max(160.0);
            let (hr, _) = ui.allocate_exact_size(egui::vec2(w, 32.0), egui::Sense::hover());
            let hf = font_sb(10.5);
            let x0 = hr.left() + 16.0;
            ui.painter().text(egui::pos2(x0, hr.center().y), egui::Align2::LEFT_CENTER, "STATUS", hf.clone(), INK_3);
            ui.painter().text(egui::pos2(x0 + st_w, hr.center().y), egui::Align2::LEFT_CENTER, "NAME", hf.clone(), INK_3);
            ui.painter().text(egui::pos2(x0 + st_w + name_w + size_w, hr.center().y), egui::Align2::RIGHT_CENTER, "SIZE", hf.clone(), INK_3);
            ui.painter().text(egui::pos2(x0 + st_w + name_w + size_w + 24.0, hr.center().y), egui::Align2::LEFT_CENTER, "NOTE", hf, INK_3);
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
                    "up-to-date" => ("Ready", GOOD),
                    "stale" => ("Out of date", WARN),
                    _ => ("Missing", INK_3),
                };
                let (r, resp) = ui.allocate_exact_size(egui::vec2(w, 30.0), egui::Sense::hover());
                let p = ui.painter();
                p.hline(r.x_range(), r.top(), egui::Stroke::new(1.0_f32, BORDER));
                if resp.hovered() {
                    p.rect_filled(r.shrink2(egui::vec2(1.0, 0.0)), 0.0, egui::Color32::from_rgb(0x22, 0x23, 0x26));
                }
                let cy = r.center().y;
                if col == INK_3 {
                    p.circle_stroke(egui::pos2(x0 + 4.0, cy), 3.0, egui::Stroke::new(1.3_f32, INK_3));
                } else {
                    p.circle_filled(egui::pos2(x0 + 4.0, cy), 3.5, col);
                }
                p.text(egui::pos2(x0 + 14.0, cy), egui::Align2::LEFT_CENTER, t, font(T_SMALL), INK_2);
                let short = c.name.rsplit('/').next().unwrap_or(&c.name);
                let g = galley_trunc(ui, short, font_m(T_SMALL), INK, name_w - 12.0);
                p.galley(egui::pos2(x0 + st_w, cy - g.size().y / 2.0), g, INK);
                p.text(egui::pos2(x0 + st_w + name_w + size_w, cy), egui::Align2::RIGHT_CENTER, format!("{:.1} MB", c.slot_mb), font(T_SMALL), INK_2);
                let nx = x0 + st_w + name_w + size_w + 24.0;
                let g = galley_trunc(ui, &c.reasons.join("; "), font(T_SMALL), INK_3, r.right() - nx - 16.0);
                p.galley(egui::pos2(nx, cy - g.size().y / 2.0), g, INK_3);
                resp.on_hover_text(format!("{}\n{}", c.name, c.reasons.join("\n")));
            }
            if shown == 0 {
                egui::Frame::new().inner_margin(16).show(ui, |ui| {
                    ui.horizontal(|ui| {
                        if self.aot.only_attention && f.is_empty() {
                            ui.label(egui::RichText::new(icon::CHECK_CIRCLE).size(16.0).color(GOOD));
                            ui.label(sub("Everything is ready"));
                        } else {
                            ui.label(sub("Nothing matches"));
                        }
                    });
                });
            }
        });
        ui.add_space(S5);
    }

    // ---------------- Settings / host ----------------
    pub fn settings_page(&mut self, ui: &mut egui::Ui) {
        page_header(ui, "Settings", "", |_| {});
        let mut save = false;
        section(ui, "Hub", |_| {});
        list_card(ui, |ui| {
            form_row(ui, "Live previews", "Show each running phone's screen on the Phones page.", false, |ui| {
                save |= toggle(ui, &mut self.hub.thumbnails).changed();
            });
            form_row(ui, "Open a phone when it starts", "Jump to its screen after pressing Start.", false, |ui| {
                save |= toggle(ui, &mut self.hub.open_on_start).changed();
            });
            form_row(ui, "Card size", "How large phones appear on the Phones page.", true, |ui| {
                save |= ui.add(egui::Slider::new(&mut self.hub.card_w, 200.0..=360.0).show_value(false)).drag_stopped();
            });
        });
        if save {
            let _ = host::save_hub(&self.hub);
        }
        ui.add_space(S5);
        let (nrun, mb) = self.running_summary();
        let gpu = host::mtlvk_pids().len();
        section(ui, "This computer", |_| {});
        stat_row(ui, CARD, &[
            ("Phones running", nrun.to_string(), ""),
            ("Memory used", if nrun > 0 { fmt_mb(mb) } else { "–".into() }, ""),
            ("GPU helpers", gpu.to_string(), ""),
            ("Free disk", if self.disk.free_gb >= 1000.0 { format!("{:.1} TB", self.disk.free_gb / 1000.0) } else { format!("{:.0} GB", self.disk.free_gb) }, ""),
        ]);
        ui.add_space(S3);
        list_card(ui, |ui| {
            let sizes = self.sizes.lock().unwrap().clone();
            let last = sizes.is_empty();
            form_row(ui, "Disk use", &format!("Caches and phone data live in {}", aot::scratch().display()), last, |ui| {
                if ui.add_enabled(!self.measuring, Btn::secondary(if self.measuring { "Measuring…" } else { "Measure" }).icon(icon::HARD_DRIVES)).clicked() {
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
            if !sizes.is_empty() {
                self.measuring = false;
                let mut v: Vec<(&PathBuf, &u64)> = sizes.iter().collect();
                v.sort_by(|a, b| b.1.cmp(a.1));
                let max = v.first().map(|x| *x.1).unwrap_or(1).max(1) as f32;
                ui.add_space(4.0);
                for (p, b) in v {
                    let name = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
                    let what = if name.starts_with("jc-") || name == "jitcache" { "Translated code" } else if name.starts_with("a64prefix") { "Phone system files" } else { "Phone data" };
                    let (r, resp) = ui.allocate_exact_size(egui::vec2(ui.available_width(), 28.0), egui::Sense::hover());
                    let pt = ui.painter();
                    let x0 = r.left() + 16.0;
                    pt.text(egui::pos2(x0, r.center().y), egui::Align2::LEFT_CENTER, &name, font_m(T_SMALL), INK);
                    pt.text(egui::pos2(x0 + 220.0, r.center().y), egui::Align2::LEFT_CENTER, what, font(T_SMALL), INK_3);
                    let bar = egui::Rect::from_min_size(egui::pos2(x0 + 380.0, r.center().y - 2.0), egui::vec2((r.right() - 16.0 - 80.0 - x0 - 380.0).max(40.0), 4.0));
                    pt.rect_filled(bar, 2.0, RAISED);
                    pt.rect_filled(egui::Rect::from_min_size(bar.min, egui::vec2((*b as f32 / max * bar.width()).max(2.0), 4.0)), 2.0, ACCENT);
                    pt.text(egui::pos2(r.right() - 16.0, r.center().y), egui::Align2::RIGHT_CENTER, format!("{:.1} GB", *b as f64 / 1e9), font(T_SMALL), INK_2);
                    resp.on_hover_text(p.display().to_string());
                }
                ui.add_space(8.0);
            }
        });
        ui.add_space(S5);
        section(ui, "Standalone window", |_| {});
        list_card(ui, |ui| {
            form_row(ui, "Viewer window", "The separate viewer (simview) still works. It follows the main phone.", true, |ui| {
                let open = sim::simview_running();
                if ui.add(Btn::secondary(if open { "Window is open" } else { "Open window" }).icon(icon::APP_WINDOW)).clicked() {
                    match sim::show() {
                        Ok(()) => self.say("Window opened"),
                        Err(e) => self.say(format!("{e:#}")),
                    }
                }
            });
        });
        ui.add_space(S5);
    }
}

/// A list row: icon tile, title + detail, trailing controls (right to left).
fn list_row(ui: &mut egui::Ui, ic: &str, title_text: &str, detail: &str, trailing: impl FnOnce(&mut egui::Ui)) {
    egui::Frame::new().inner_margin(egui::Margin::symmetric(16, 12)).show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.horizontal(|ui| {
            let (r, _) = ui.allocate_exact_size(egui::vec2(32.0, 32.0), egui::Sense::hover());
            ui.painter().rect_filled(r, 8.0, RAISED);
            ui.painter().text(r.center(), egui::Align2::CENTER_CENTER, ic, font(17.0), INK_2);
            ui.add_space(4.0);
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 1.0;
                ui.label(strong(title_text));
                ui.label(muted(detail));
            });
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), trailing);
        });
    });
}

/// The last line of a running download's log, under its row.
fn fetch_tail(ui: &mut egui::Ui, build: &str) {
    let tail = std::fs::read_to_string(host::fetch_log(build)).ok().and_then(|s| s.lines().filter(|l| !l.trim().is_empty()).last().map(|l| l.to_string())).unwrap_or_default();
    egui::Frame::new().inner_margin(egui::Margin { left: 60, right: 16, top: 0, bottom: 12 }).show(ui, |ui| {
        ui.add(egui::Label::new(mono(tail)).truncate());
    });
    ui.ctx().request_repaint_after(Duration::from_secs(2));
}
