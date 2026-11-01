//! x86Code hub: every phone (var/device-a64*) as a card with its live screen, open one for its embedded screen with
//! touch and keyboard plus a fold-out panel (features, logs, performance, advanced); global pages for iOS versions,
//! pre-translation (AOT) and settings. Logic lives in x86code-core; the GUI repaints only when a frame or a status
//! changes.
mod global;
mod home;
mod phone;
mod theme;
mod viewer;

use eframe::egui;
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use theme::*;
use x86code_core::{host, instance, logs};

fn main() -> eframe::Result<()> {
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([1280.0, 820.0]).with_min_inner_size([900.0, 600.0]).with_title("x86Code"),
        ..Default::default()
    };
    eframe::run_native("x86Code", opts, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}

#[derive(Default)]
pub struct Shared {
    pub insts: Vec<instance::Instance>,
    pub status: HashMap<String, instance::Status>,
    pub monitor: instance::Monitor,
    pub polled: bool,
    pub fetches: Vec<host::Fetch>,
}

#[derive(Clone, PartialEq)]
pub enum View {
    Home,
    Phone(String),
    Ios,
    Aot,
    Settings,
}

pub struct AddDialog {
    pub model: usize,
    pub runtime: usize,
    pub name: String,
    pub busy: Arc<Mutex<Option<Result<std::path::PathBuf, String>>>>,
    pub working: bool,
}

/// A phone dialog: rename, or confirm removing it.
pub enum Dialog {
    Rename { id: String, text: String },
    /// size: what it frees (measured in the background)
    Delete { id: String, paths: Vec<std::path::PathBuf>, size: Arc<Mutex<Option<u64>>> },
}

pub struct App {
    /// fresh phones already restarted once after a stalled first boot
    pub first_boot_retried: HashSet<String>,
    pub dialog: Option<Dialog>,
    pub shared: Arc<Mutex<Shared>>,
    pub insts: Vec<instance::Instance>,
    pub status: HashMap<String, instance::Status>,
    pub view: View,
    pub viewers: HashMap<String, viewer::Viewer>,
    pub hub: host::HubSettings,
    pub open: HashSet<&'static str>,
    pub add: Option<AddDialog>,
    pub models: Vec<(String, String)>,
    pub message: String,
    pub message_at: Instant,
    pub logs: Option<(String, logs::Tail)>,
    pub log_filter: String,
    pub log_source: String,
    pub log_paused: bool,
    pub last_log_poll: Instant,
    pub settings: Option<(String, instance::BootSettings)>,
    pub aot: global::Aot,
    pub runtimes: Vec<host::Runtime>,
    pub catalogue: Arc<Mutex<Option<Vec<host::Catalogue>>>>,
    /// downloads in progress by build (the poller's view, plus ones just started)
    pub fetching: HashMap<String, host::Fetch>,
    pub fetch_started: HashMap<String, Instant>,
    pub hub_show_betas: bool,
    /// phones whose separate viewer window is open, and the GPU helper count (checked every few seconds, not per frame)
    pub simview_open: HashSet<String>,
    pub gpu_helpers: usize,
    pub sizes: Arc<Mutex<HashMap<std::path::PathBuf, u64>>>,
    pub measuring: bool,
    pub disk: host::Disk,
    pub last_slow: Instant,
    pub ctx: egui::Context,
}

impl App {
    fn new(cc: &eframe::CreationContext) -> App {
        theme(&cc.egui_ctx);
        let shared = Arc::new(Mutex::new(Shared::default()));
        // status of every phone every 2 s; one repaint per poll
        let sh = shared.clone();
        let ctx = cc.egui_ctx.clone();
        std::thread::spawn(move || loop {
            let insts = instance::list();
            {
                let mut g = sh.lock().unwrap();
                let st = g.monitor.poll(&insts);
                g.status = st.into_iter().map(|s| (s.id.clone(), s)).collect();
                g.insts = insts;
                g.polled = true;
                g.fetches = host::fetches();
            }
            ctx.request_repaint();
            std::thread::sleep(Duration::from_secs(2));
        });
        let runtimes = host::runtimes();
        let catalogue = Arc::new(Mutex::new(None));
        let c2 = catalogue.clone();
        std::thread::spawn(move || {
            let v = host::catalogue(false);
            *c2.lock().unwrap() = Some(v);
        });
        let aot = global::Aot::new(runtimes.iter().find(|r| r.present).map(|r| r.root.clone()));
        // start view / open sections, for scripted screenshots: X86CODE_LAUNCHER_VIEW=ios|aot|settings|add|phone:<id>,
        // X86CODE_LAUNCHER_OPEN=features,logs,perf,adv
        let view = match std::env::var("X86CODE_LAUNCHER_VIEW").unwrap_or_default().as_str() {
            "ios" => View::Ios,
            "aot" => View::Aot,
            "settings" => View::Settings,
            v if v.starts_with("phone:") => View::Phone(v[6..].to_string()),
            _ => View::Home,
        };
        let mut open = HashSet::new();
        for k in std::env::var("X86CODE_LAUNCHER_OPEN").unwrap_or_default().split(',') {
            for key in ["features", "logs", "perf", "adv"] {
                if k == key {
                    open.insert(key);
                }
            }
        }
        let add_on_start = std::env::var("X86CODE_LAUNCHER_VIEW").is_ok_and(|v| v == "add");
        let mut app = App {
            dialog: None,
            first_boot_retried: HashSet::new(),
            shared,
            insts: instance::list(),
            status: HashMap::new(),
            view,
            viewers: HashMap::new(),
            hub: host::load_hub(),
            open,
            add: None,
            models: x86code_core::sim::models(),
            message: String::new(),
            message_at: Instant::now(),
            logs: None,
            log_filter: String::new(),
            log_source: String::new(),
            log_paused: false,
            last_log_poll: Instant::now(),
            settings: None,
            aot,
            runtimes,
            catalogue,
            fetching: HashMap::new(),
            fetch_started: HashMap::new(),
            hub_show_betas: false,
            simview_open: HashSet::new(),
            gpu_helpers: 0,
            sizes: Arc::new(Mutex::new(HashMap::new())),
            measuring: false,
            disk: host::disk(&x86code_core::aot::scratch()),
            last_slow: Instant::now() - Duration::from_secs(60),
            ctx: cc.egui_ctx.clone(),
        };
        if add_on_start {
            app.open_add_dialog();
        }
        app
    }

    pub fn say(&mut self, m: impl Into<String>) {
        self.message = m.into();
        self.message_at = Instant::now();
    }

    pub fn inst(&self, id: &str) -> Option<&instance::Instance> {
        self.insts.iter().find(|i| i.id == id)
    }

    pub fn phase(&self, id: &str) -> instance::Phase {
        self.status.get(id).map(|s| s.phase).unwrap_or(instance::Phase::Off)
    }

    pub fn viewer(&mut self, id: &str) -> Option<&mut viewer::Viewer> {
        if !self.viewers.contains_key(id) {
            let inst = self.inst(id)?.clone();
            self.viewers.insert(id.to_string(), viewer::Viewer::new(&inst, self.ctx.clone()));
        }
        let bt = self.status.get(id).and_then(|s| s.boot_time);
        let ring = self.inst(id)?.ring.clone();
        let v = self.viewers.get_mut(id)?;
        v.set_ring(&ring);
        v.set_boot_time(bt);
        Some(v)
    }

    pub fn start(&mut self, id: &str) {
        let Some(inst) = self.inst(id).cloned() else { return };
        let others: Vec<instance::Status> = self.status.values().cloned().collect();
        match instance::start(&inst, &others) {
            Ok(_) => {
                self.shared.lock().unwrap().monitor.starting.insert(id.to_string(), Instant::now());
                self.status.entry(id.to_string()).and_modify(|s| s.phase = instance::Phase::Starting);
                self.say(format!("Starting {}", inst.name));
                if self.hub.open_on_start {
                    self.view = View::Phone(id.to_string());
                }
            }
            Err(e) => self.say(format!("Couldn't start: {e:#}")),
        }
    }

    pub fn stop(&mut self, id: &str) {
        let Some(inst) = self.inst(id).cloned() else { return };
        let st = self.status.get(id).cloned();
        self.shared.lock().unwrap().monitor.stopping.insert(id.to_string(), Instant::now());
        self.status.entry(id.to_string()).and_modify(|s| s.phase = instance::Phase::Stopping);
        self.say(format!("Stopping {}", inst.name));
        std::thread::spawn(move || {
            let _ = instance::stop(&inst, st.as_ref());
        });
    }

    pub fn restart(&mut self, id: &str) {
        let Some(inst) = self.inst(id).cloned() else { return };
        let st = self.status.get(id).cloned();
        let others: Vec<instance::Status> = self.status.values().filter(|s| s.id != id).cloned().collect();
        let sh = self.shared.clone();
        sh.lock().unwrap().monitor.stopping.insert(id.to_string(), Instant::now());
        self.status.entry(id.to_string()).and_modify(|s| s.phase = instance::Phase::Stopping);
        self.say(format!("Restarting {}", inst.name));
        let id = id.to_string();
        std::thread::spawn(move || {
            let _ = instance::stop(&inst, st.as_ref());
            let _ = instance::start(&inst, &others);
            let mut g = sh.lock().unwrap();
            g.monitor.stopping.remove(&id);
            g.monitor.starting.insert(id, Instant::now());
        });
    }

    pub fn refresh_instances(&mut self) {
        self.insts = instance::list();
        self.runtimes = host::runtimes();
    }

    fn sync(&mut self) {
        let g = self.shared.lock().unwrap();
        if g.polled {
            // the poller's view of the phones is the truth (a boot rewrites sim.env: ring, model, name)
            self.insts = g.insts.clone();
            self.status = g.status.clone();
            // downloads: what runs, plus ones started in the last seconds that the poller hasn't seen yet
            let mut f: HashMap<String, host::Fetch> = g.fetches.iter().map(|f| (f.build.clone(), f.clone())).collect();
            self.fetch_started.retain(|_, t| t.elapsed() < Duration::from_secs(15));
            for (b, _) in self.fetch_started.iter() {
                f.entry(b.clone()).or_insert_with(|| host::Fetch { build: b.clone(), phase: "starting".into(), ..Default::default() });
            }
            let finished = self.fetching.keys().any(|b| !f.contains_key(b));
            self.fetching = f;
            if finished {
                self.runtimes = host::runtimes();
                self.insts = instance::list();
            }
        }
    }

    fn running_summary(&self) -> (usize, f64) {
        let n = self.status.values().filter(|s| s.phase != instance::Phase::Off).count();
        let mb: f64 = self.status.values().map(|s| s.pss_mb).sum();
        (n, mb)
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.sync();
        if self.last_slow.elapsed() > Duration::from_secs(10) {
            self.last_slow = Instant::now();
            self.disk = host::disk(&x86code_core::aot::scratch());
            if self.view == View::Settings {
                self.gpu_helpers = host::mtlvk_pids().len();
                self.simview_open = self.insts.iter().filter(|i| self.status.get(&i.id).is_some_and(|s| s.phase != instance::Phase::Off) && x86code_core::sim::simview_running_for(i)).map(|i| i.id.clone()).collect();
            }
            if self.view == View::Ios {
                self.runtimes = host::runtimes();
            }
            ctx.request_repaint_after(Duration::from_secs(10));
        }
        self.aot.tick();
        if self.aot.building() {
            ctx.request_repaint_after(Duration::from_millis(500));
        }
        // a new phone's first boot can stall on iOS's app database (known; the second boot works): restart it once
        let stalled: Vec<String> = self.status.values().filter(|s| instance::first_boot_stalled(s) && !self.first_boot_retried.contains(&s.id)).map(|s| s.id.clone()).collect();
        for id in stalled {
            self.first_boot_retried.insert(id.clone());
            let name = self.inst(&id).map(|i| i.name.clone()).unwrap_or_default();
            self.restart(&id);
            self.say(format!("{name}'s first start stalled while iOS set up its apps. Restarting it once (this is expected for a new phone)."));
        }
        // thumbnails and the open phone's screen only: everything else sleeps
        let open_id = if let View::Phone(id) = &self.view { Some(id.clone()) } else { None };
        for (id, v) in self.viewers.iter_mut() {
            if Some(id) != open_id.as_ref() && !(self.view == View::Home && self.hub.thumbnails) {
                v.shared.mode.store(viewer::OFF, std::sync::atomic::Ordering::Relaxed);
            }
        }

        let (nrun, mb) = self.running_summary();
        egui::SidePanel::left("nav")
            .resizable(false)
            .exact_width(212.0)
            .frame(egui::Frame::new().fill(WINDOW).inner_margin(egui::Margin { left: 10, right: 10, top: 16, bottom: 14 }).stroke(egui::Stroke::new(1.0_f32, BORDER)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.add_space(6.0);
                    let (r, _) = ui.allocate_exact_size(egui::vec2(28.0, 28.0), egui::Sense::hover());
                    ui.painter().rect_filled(r, 7.0, ACCENT_FILL);
                    ui.painter().text(r.center(), egui::Align2::CENTER_CENTER, icon::DEVICE_MOBILE, font(17.0), egui::Color32::WHITE);
                    ui.add_space(2.0);
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(egui::RichText::new("x86Code").size(T_LG).family(semibold()).color(INK));
                        ui.label(muted("iPhone simulator").size(T_CAPTION));
                    });
                });
                ui.add_space(S5);
                ui.spacing_mut().item_spacing.y = 2.0;
                let n = self.insts.len();
                let on_phones = matches!(self.view, View::Home | View::Phone(_));
                if nav_item(ui, icon::SQUARES_FOUR, "Phones", on_phones, &if n > 0 { n.to_string() } else { String::new() }) {
                    self.view = View::Home;
                }
                if nav_item(ui, icon::DOWNLOAD_SIMPLE, "iOS versions", self.view == View::Ios, "") {
                    self.view = View::Ios;
                    self.runtimes = host::runtimes();
                }
                if nav_item(ui, icon::LIGHTNING, "Pre-translate", self.view == View::Aot, if self.aot.building() { "•" } else { "" }) {
                    self.view = View::Aot;
                }
                if nav_item(ui, icon::GEAR_SIX, "Settings", self.view == View::Settings, "") {
                    self.view = View::Settings;
                    self.last_slow = Instant::now() - Duration::from_secs(60);
                }
                // footer: host status, pinned to the bottom
                let footer_h = 40.0 + if self.aot.building() { 30.0 } else { 0.0 };
                ui.add_space((ui.available_height() - footer_h).max(S4));
                egui::Frame::new().inner_margin(egui::Margin::symmetric(8, 0)).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.spacing_mut().item_spacing.y = 4.0;
                    if self.aot.building() {
                        let frac = self.aot.progress.as_ref().map(|p| if p.chunks_total > 0 { p.chunks_done as f32 / p.chunks_total as f32 } else { 0.0 }).unwrap_or(0.0);
                        ui.label(muted(format!("Pre-translating · {:.0}%", frac * 100.0)));
                        ui.add(egui::ProgressBar::new(frac).desired_height(4.0).fill(ACCENT).corner_radius(2));
                        ui.add_space(4.0);
                    }
                    if nrun > 0 {
                        dot_label(ui, &format!("{nrun} running · {}", fmt_mb(mb)), GOOD);
                    } else {
                        dot_label(ui, "Nothing running", INK_3);
                    }
                    ui.label(muted(format!("{} free for caches", if self.disk.free_gb >= 1000.0 { format!("{:.1} TB", self.disk.free_gb / 1000.0) } else { format!("{:.0} GB", self.disk.free_gb) })));
                });
            });

        let view = self.view.clone();
        match view {
            View::Phone(id) => self.phone_view(ctx, &id),
            _ => {
                egui::CentralPanel::default().frame(egui::Frame::new().fill(CANVAS)).show(ctx, |ui| {
                    egui::ScrollArea::vertical().id_salt(match &view { View::Home => "home", View::Ios => "ios", View::Aot => "aot", _ => "settings" }).auto_shrink([false, false]).show(ui, |ui| {
                        // the page owns its margins so the scrollbar sits in the gutter, not on the content
                        egui::Frame::new().inner_margin(egui::Margin { left: S6 as i8, right: S6 as i8, top: S5 as i8, bottom: S6 as i8 }).show(ui, |ui| {
                            ui.set_width(ui.available_width());
                            match view {
                                View::Home => self.home(ui),
                                View::Ios => self.ios_page(ui),
                                View::Aot => self.aot_page(ui),
                                View::Settings => self.settings_page(ui),
                                View::Phone(_) => {}
                            }
                        });
                    });
                });
            }
        }
        self.add_dialog(ctx);
        self.phone_dialog(ctx);
        if !self.message.is_empty() && self.message_at.elapsed() < Duration::from_secs(6) {
            egui::Area::new("toast".into()).anchor(egui::Align2::CENTER_BOTTOM, egui::vec2(106.0, -24.0)).order(egui::Order::Foreground).show(ctx, |ui| {
                egui::Frame::new()
                    .fill(RAISED)
                    .stroke(egui::Stroke::new(1.0_f32, BORDER_STRONG))
                    .corner_radius(8)
                    .shadow(egui::Shadow { offset: [0, 6], blur: 20, spread: 0, color: egui::Color32::from_black_alpha(120) })
                    .inner_margin(egui::Margin::symmetric(14, 9))
                    .show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(icon::INFO).size(15.0).color(INK_2));
                            ui.label(body(&self.message));
                        });
                    });
            });
            ctx.request_repaint_after(Duration::from_secs(6));
        }
    }
}
