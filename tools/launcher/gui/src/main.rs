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

pub struct App {
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
    pub fetching: HashMap<String, u32>,
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
        App {
            shared,
            insts: instance::list(),
            status: HashMap::new(),
            view: View::Home,
            viewers: HashMap::new(),
            hub: host::load_hub(),
            open: HashSet::new(),
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
            sizes: Arc::new(Mutex::new(HashMap::new())),
            measuring: false,
            disk: host::disk(&x86code_core::aot::scratch()),
            last_slow: Instant::now(),
            ctx: cc.egui_ctx.clone(),
        }
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
        let v = self.viewers.get_mut(id)?;
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
            // the poller's view of the phones is the truth; keep card order stable
            if g.insts.len() != self.insts.len() || g.insts.iter().zip(&self.insts).any(|(a, b)| a.id != b.id || a.runtime_present != b.runtime_present) {
                self.insts = g.insts.clone();
            }
            self.status = g.status.clone();
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
            self.fetching.retain(|_, pid| std::path::Path::new(&format!("/proc/{pid}")).exists());
            if self.view == View::Ios {
                self.runtimes = host::runtimes();
            }
            ctx.request_repaint_after(Duration::from_secs(10));
        }
        self.aot.tick();
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
            .exact_width(224.0)
            .frame(egui::Frame::new().fill(RAIL).inner_margin(egui::Margin { left: 12, right: 12, top: 18, bottom: 14 }).stroke(egui::Stroke::new(1.0_f32, BORDER)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    let (r, p) = ui.allocate_painter(egui::vec2(32.0, 32.0), egui::Sense::hover());
                    p.rect_filled(r.rect, 9.0, ACCENT);
                    p.text(r.rect.center(), egui::Align2::CENTER_CENTER, "x86", egui::FontId::proportional(12.0), egui::Color32::WHITE);
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(egui::RichText::new("x86Code").size(16.0).strong());
                        ui.label(muted("iPhone hub").size(11.5));
                    });
                });
                ui.add_space(18.0);
                ui.spacing_mut().item_spacing.y = 2.0;
                if nav_item(ui, "▦", "Phones", self.view == View::Home, None) {
                    self.view = View::Home;
                }
                let items: Vec<(String, String, instance::Phase)> = self.insts.iter().map(|i| (i.id.clone(), i.name.clone(), self.phase(&i.id))).collect();
                for (id, name, ph) in items {
                    let dot = match ph {
                        instance::Phase::Running => Some(GREEN),
                        instance::Phase::Starting | instance::Phase::Stopping => Some(ORANGE),
                        instance::Phase::Off => None,
                    };
                    ui.horizontal(|ui| {
                        ui.add_space(10.0);
                        if nav_item(ui, "📱", &name, self.view == View::Phone(id.clone()), dot) {
                            self.view = View::Phone(id.clone());
                        }
                    });
                }
                ui.add_space(12.0);
                hline(ui);
                ui.add_space(10.0);
                if nav_item(ui, "⬇", "iOS versions", self.view == View::Ios, None) {
                    self.view = View::Ios;
                    self.runtimes = host::runtimes();
                }
                if nav_item(ui, "⚡", "Pre-translate", self.view == View::Aot, None) {
                    self.view = View::Aot;
                }
                if nav_item(ui, "⚙", "Settings", self.view == View::Settings, None) {
                    self.view = View::Settings;
                }
                ui.with_layout(egui::Layout::bottom_up(egui::Align::Min), |ui| {
                    ui.spacing_mut().item_spacing.y = 4.0;
                    ui.label(muted(format!("{:.0} GB free for caches", self.disk.free_gb)).size(11.5));
                    if nrun > 0 {
                        ui.label(muted(format!("{nrun} running · {}", fmt_mb(mb))).size(11.5));
                    } else {
                        ui.label(muted("Nothing running").size(11.5));
                    }
                    if self.aot.building() {
                        let frac = self.aot.progress.as_ref().map(|p| if p.chunks_total > 0 { p.chunks_done as f32 / p.chunks_total as f32 } else { 0.0 }).unwrap_or(0.0);
                        ui.label(muted(format!("Pre-translating… {:.0}%", frac * 100.0)).size(11.5));
                        ui.add(egui::ProgressBar::new(frac).desired_height(5.0).fill(ACCENT).corner_radius(3));
                    }
                });
            });

        let view = self.view.clone();
        match view {
            View::Phone(id) => self.phone_view(ctx, &id),
            _ => {
                egui::CentralPanel::default().frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::symmetric(24, 14))).show(ctx, |ui| {
                    egui::ScrollArea::vertical().id_salt(match &view { View::Home => "home", View::Ios => "ios", View::Aot => "aot", _ => "settings" }).auto_shrink([false, false]).show(ui, |ui| match view {
                        View::Home => self.home(ui),
                        View::Ios => self.ios_page(ui),
                        View::Aot => self.aot_page(ui),
                        View::Settings => self.settings_page(ui),
                        View::Phone(_) => {}
                    });
                });
            }
        }
        self.add_dialog(ctx);
        if !self.message.is_empty() && self.message_at.elapsed() < Duration::from_secs(6) {
            egui::Area::new("toast".into()).anchor(egui::Align2::CENTER_BOTTOM, egui::vec2(100.0, -18.0)).order(egui::Order::Foreground).show(ctx, |ui| {
                egui::Frame::new().fill(CARD2).stroke(egui::Stroke::new(1.0_f32, BORDER)).corner_radius(10).inner_margin(egui::Margin::symmetric(14, 8)).show(ui, |ui| {
                    ui.label(&self.message);
                });
            });
            ctx.request_repaint_after(Duration::from_secs(6));
        }
    }
}
