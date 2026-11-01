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
        viewport: egui::ViewportBuilder::default().with_inner_size([1120.0, 780.0]).with_min_inner_size([860.0, 560.0]).with_title("x86Code launcher"),
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
    page: Page,
    models: Vec<(String, String)>,
    adding: bool,
    new_model: usize,
    new_name: String,
}

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Simulator,
    Features,
    Aot,
}

impl App {
    fn new(cc: &eframe::CreationContext) -> App {
        theme(&cc.egui_ctx);
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
            page: Page::Simulator,
            models: sim::models(),
            adding: false,
            new_model: 0,
            new_name: String::new(),
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

// ---- look ----

const BG: egui::Color32 = egui::Color32::from_rgb(0x0e, 0x10, 0x14);
const RAIL: egui::Color32 = egui::Color32::from_rgb(0x13, 0x16, 0x1c);
const CARD: egui::Color32 = egui::Color32::from_rgb(0x19, 0x1d, 0x25);
const CARD2: egui::Color32 = egui::Color32::from_rgb(0x21, 0x26, 0x30);
const BORDER: egui::Color32 = egui::Color32::from_rgb(0x2a, 0x30, 0x3c);
const TEXT: egui::Color32 = egui::Color32::from_rgb(0xe8, 0xeb, 0xf1);
const MUTED: egui::Color32 = egui::Color32::from_rgb(0x8c, 0x94, 0xa6);
const ACCENT: egui::Color32 = egui::Color32::from_rgb(0x4c, 0x8d, 0xff);
const GREEN: egui::Color32 = egui::Color32::from_rgb(0x3d, 0xd6, 0x8c);
const ORANGE: egui::Color32 = egui::Color32::from_rgb(0xff, 0xb0, 0x4c);
const RED: egui::Color32 = egui::Color32::from_rgb(0xff, 0x6b, 0x6b);

fn theme(ctx: &egui::Context) {
    use egui::{FontFamily, FontId, TextStyle};
    ctx.set_theme(egui::Theme::Dark);
    ctx.style_mut(|s| {
        s.text_styles = [
            (TextStyle::Heading, FontId::new(24.0, FontFamily::Proportional)),
            (TextStyle::Body, FontId::new(14.5, FontFamily::Proportional)),
            (TextStyle::Button, FontId::new(14.5, FontFamily::Proportional)),
            (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
            (TextStyle::Monospace, FontId::new(13.0, FontFamily::Monospace)),
        ]
        .into();
        s.spacing.item_spacing = egui::vec2(10.0, 8.0);
        s.spacing.button_padding = egui::vec2(14.0, 7.0);
        s.spacing.interact_size.y = 32.0;
        s.spacing.slider_width = 180.0;
        let v = &mut s.visuals;
        v.panel_fill = BG;
        v.window_fill = CARD;
        v.extreme_bg_color = CARD2;
        v.faint_bg_color = egui::Color32::from_rgb(0x1d, 0x22, 0x2b);
        v.override_text_color = Some(TEXT);
        v.hyperlink_color = ACCENT;
        v.selection.bg_fill = ACCENT.gamma_multiply(0.35);
        v.selection.stroke = egui::Stroke::new(1.0_f32, ACCENT);
        v.window_corner_radius = 12.into();
        v.menu_corner_radius = 10.into();
        v.window_stroke = egui::Stroke::new(1.0_f32, BORDER);
        v.slider_trailing_fill = true;
        let r: egui::CornerRadius = 8.into();
        for w in [&mut v.widgets.noninteractive, &mut v.widgets.inactive, &mut v.widgets.hovered, &mut v.widgets.active, &mut v.widgets.open] {
            w.corner_radius = r;
        }
        v.widgets.noninteractive.bg_stroke = egui::Stroke::new(1.0_f32, BORDER);
        v.widgets.noninteractive.fg_stroke = egui::Stroke::new(1.0_f32, TEXT);
        v.widgets.inactive.bg_fill = CARD2;
        v.widgets.inactive.weak_bg_fill = CARD2;
        v.widgets.inactive.bg_stroke = egui::Stroke::new(1.0_f32, BORDER);
        v.widgets.inactive.fg_stroke = egui::Stroke::new(1.0_f32, TEXT);
        v.widgets.hovered.bg_fill = egui::Color32::from_rgb(0x2a, 0x31, 0x3d);
        v.widgets.hovered.weak_bg_fill = egui::Color32::from_rgb(0x2a, 0x31, 0x3d);
        v.widgets.hovered.bg_stroke = egui::Stroke::new(1.0_f32, ACCENT.gamma_multiply(0.6));
        v.widgets.hovered.expansion = 0.0;
        v.widgets.active.bg_fill = egui::Color32::from_rgb(0x33, 0x3b, 0x49);
        v.widgets.active.weak_bg_fill = egui::Color32::from_rgb(0x33, 0x3b, 0x49);
        v.widgets.active.bg_stroke = egui::Stroke::new(1.0_f32, ACCENT);
        v.widgets.active.expansion = 0.0;
    });
}

fn card<R>(ui: &mut egui::Ui, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    egui::Frame::new()
        .fill(CARD)
        .stroke(egui::Stroke::new(1.0_f32, BORDER))
        .corner_radius(12)
        .inner_margin(18)
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            add(ui)
        })
        .inner
}

fn section(ui: &mut egui::Ui, title: &str) {
    ui.label(egui::RichText::new(title).size(16.0).strong());
    ui.add_space(2.0);
}

fn muted(t: impl Into<String>) -> egui::RichText {
    egui::RichText::new(t).color(MUTED)
}

fn page_header(ui: &mut egui::Ui, title: &str, sub: &str) {
    ui.add_space(8.0);
    ui.label(egui::RichText::new(title).size(26.0).strong());
    ui.label(muted(sub));
    ui.add_space(12.0);
}

fn primary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(egui::Color32::WHITE).strong()).fill(ACCENT).stroke(egui::Stroke::NONE).min_size(egui::vec2(120.0, 36.0))
}

fn secondary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(text.into()).min_size(egui::vec2(96.0, 36.0))
}

fn pill(ui: &mut egui::Ui, text: &str, col: egui::Color32) {
    egui::Frame::new().fill(col.gamma_multiply(0.16)).corner_radius(20).inner_margin(egui::Margin::symmetric(10, 3)).show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 6.0;
            let (r, p) = ui.allocate_painter(egui::vec2(8.0, 8.0), egui::Sense::hover());
            p.circle_filled(r.rect.center(), 4.0, col);
            ui.label(egui::RichText::new(text).color(col).size(12.5).strong());
        });
    });
}

fn stat(ui: &mut egui::Ui, w: f32, label: &str, value: String, col: egui::Color32) {
    egui::Frame::new().fill(CARD2).corner_radius(10).inner_margin(14).show(ui, |ui| {
        ui.set_width(w - 28.0);
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 2.0;
            ui.label(muted(label).size(12.0));
            ui.label(egui::RichText::new(value).size(22.0).strong().color(col));
        });
    });
}

fn stat_row(ui: &mut egui::Ui, items: Vec<(&str, String, egui::Color32)>) {
    let gap = 12.0;
    let w = (ui.available_width() - gap * (items.len() as f32 - 1.0)) / items.len() as f32;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = gap;
        for (l, v, c) in items {
            stat(ui, w, l, v, c);
        }
    });
}

/// iOS-style switch
fn toggle(ui: &mut egui::Ui, on: &mut bool) -> egui::Response {
    let size = egui::vec2(42.0, 24.0);
    let (rect, mut resp) = ui.allocate_exact_size(size, egui::Sense::click());
    if resp.clicked() {
        *on = !*on;
        resp.mark_changed();
    }
    let t = ui.ctx().animate_bool_responsive(resp.id, *on);
    let bg = egui::Color32::from_rgb(0x3a, 0x41, 0x4f).lerp_to_gamma(GREEN, t);
    let p = ui.painter();
    p.rect_filled(rect, 12.0, bg);
    let x = egui::lerp((rect.left() + 12.0)..=(rect.right() - 12.0), t);
    p.circle_filled(egui::pos2(x, rect.center().y), 9.5, egui::Color32::WHITE);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// segmented control; returns the clicked index
fn segmented(ui: &mut egui::Ui, items: &[&str], sel: Option<usize>, enabled: bool) -> Option<usize> {
    let mut out = None;
    egui::Frame::new().fill(CARD2).corner_radius(10).inner_margin(3).show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 2.0;
            for (i, l) in items.iter().enumerate() {
                let on = sel == Some(i);
                let b = egui::Button::new(egui::RichText::new(*l).color(if on { egui::Color32::WHITE } else { MUTED }))
                    .fill(if on { ACCENT } else { egui::Color32::TRANSPARENT })
                    .stroke(egui::Stroke::NONE)
                    .corner_radius(8)
                    .min_size(egui::vec2(0.0, 30.0));
                if ui.add_enabled(enabled, b).clicked() {
                    out = Some(i);
                }
            }
        });
    });
    out
}

fn nav_item(ui: &mut egui::Ui, icon: &str, label: &str, on: bool) -> bool {
    let w = ui.available_width();
    let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, 40.0), egui::Sense::click());
    let p = ui.painter();
    if on {
        p.rect_filled(rect, 10.0, ACCENT.gamma_multiply(0.18));
        p.rect_filled(egui::Rect::from_min_size(rect.left_top() + egui::vec2(0.0, 10.0), egui::vec2(3.0, 20.0)), 2.0, ACCENT);
    } else if resp.hovered() {
        p.rect_filled(rect, 10.0, CARD2);
    }
    let col = if on { TEXT } else { MUTED };
    p.text(rect.left_center() + egui::vec2(16.0, 0.0), egui::Align2::LEFT_CENTER, icon, egui::FontId::proportional(16.0), if on { ACCENT } else { MUTED });
    p.text(rect.left_center() + egui::vec2(44.0, 0.0), egui::Align2::LEFT_CENTER, label, egui::FontId::proportional(15.0), col);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand).clicked()
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        ctx.request_repaint_after(Duration::from_millis(500));
        let building = self.building();
        self.poll_progress();
        let (simst, status, status_busy) = {
            let g = self.shared.lock().unwrap();
            (g.sim.clone(), g.status.clone(), g.status_busy)
        };
        let booted = simst.as_ref().is_some_and(|s| s.booted);

        egui::SidePanel::left("nav")
            .resizable(false)
            .exact_width(220.0)
            .frame(egui::Frame::new().fill(RAIL).inner_margin(egui::Margin { left: 14, right: 14, top: 20, bottom: 16 }).stroke(egui::Stroke::new(1.0_f32, BORDER)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    let (r, p) = ui.allocate_painter(egui::vec2(34.0, 34.0), egui::Sense::hover());
                    p.rect_filled(r.rect, 9.0, ACCENT);
                    p.text(r.rect.center(), egui::Align2::CENTER_CENTER, "x86", egui::FontId::proportional(12.5), egui::Color32::WHITE);
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(egui::RichText::new("x86Code").size(17.0).strong());
                        ui.label(muted("iOS Simulator").size(12.0));
                    });
                });
                ui.add_space(22.0);
                ui.spacing_mut().item_spacing.y = 4.0;
                for (pg, icon, l) in [(Page::Simulator, "📱", "Simulator"), (Page::Features, "☰", "Features"), (Page::Aot, "⚡", "Pre-translate")] {
                    if nav_item(ui, icon, l, self.page == pg) {
                        self.page = pg;
                    }
                }
                ui.with_layout(egui::Layout::bottom_up(egui::Align::Min), |ui| {
                    if building {
                        let frac = self.progress.as_ref().map(|p| if p.chunks_total > 0 { p.chunks_done as f32 / p.chunks_total as f32 } else { 0.0 }).unwrap_or(0.0);
                        ui.add(egui::ProgressBar::new(frac).desired_height(6.0).fill(ACCENT).corner_radius(3));
                        ui.label(muted(format!("Preparing… {:.0}%", frac * 100.0)).size(12.0));
                        ui.add_space(6.0);
                    }
                    match &simst {
                        Some(s) if s.home => pill(ui, "Running", GREEN),
                        Some(s) if s.booted => pill(ui, "Starting", ORANGE),
                        Some(_) => pill(ui, "Stopped", MUTED),
                        None => pill(ui, "Checking", MUTED),
                    }
                });
            });

        egui::CentralPanel::default().frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::symmetric(28, 16))).show(ctx, |ui| {
            egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                // keep content at a readable width, centred on wide windows
                let w = ui.available_width().min(900.0);
                let pad = (ui.available_width() - w) / 2.0;
                ui.horizontal(|ui| {
                    ui.add_space(pad);
                    ui.vertical(|ui| {
                        ui.set_width(w);
                        match self.page {
                            Page::Simulator => self.page_sim(ui, &simst, booted),
                            Page::Features => self.page_features(ui, booted),
                            Page::Aot => self.page_aot(ui, building, &status, status_busy),
                        }
                        ui.add_space(24.0);
                    });
                });
            });
        });
    }
}

impl App {
    fn page_sim(&mut self, ui: &mut egui::Ui, simst: &Option<sim::SimStatus>, booted: bool) {
        page_header(ui, "Simulator", "Start a phone and open its window.");
        card(ui, |ui| {
            section(ui, "Phone");
            egui::Grid::new("simgrid").num_columns(2).spacing([16.0, 10.0]).show(ui, |ui| {
                ui.label(muted("iOS version"));
                egui::ComboBox::from_id_salt("rt").width(320.0).selected_text(self.runtimes.get(self.runtime).map(|r| r.0.clone()).unwrap_or("none".into())).show_ui(ui, |ui| {
                    for (i, r) in self.runtimes.iter().enumerate() {
                        ui.selectable_value(&mut self.runtime, i, &r.0);
                    }
                });
                ui.end_row();
                ui.label(muted("Model"));
                egui::ComboBox::from_id_salt("dev").width(320.0).selected_text(self.devices.get(self.device).map(|d| d.0.clone()).unwrap_or("none".into())).show_ui(ui, |ui| {
                    for (i, d) in self.devices.iter().enumerate() {
                        ui.selectable_value(&mut self.device, i, &d.0);
                    }
                });
                ui.end_row();
                if self.adding {
                    ui.label(muted("New phone"));
                    ui.horizontal(|ui| {
                        egui::ComboBox::from_id_salt("newmodel").width(190.0).selected_text(self.models.get(self.new_model).map(|m| m.0.clone()).unwrap_or_default()).show_ui(ui, |ui| {
                            for (i, m) in self.models.iter().enumerate() {
                                ui.selectable_value(&mut self.new_model, i, &m.0);
                            }
                        });
                        ui.add(egui::TextEdit::singleline(&mut self.new_name).hint_text("Name (optional)").desired_width(120.0))
                            .on_hover_text("Makes var/device-a64-<name> with this model's screen and settings (copied from the main phone's data)");
                    });
                    ui.end_row();
                    ui.label("");
                    ui.horizontal(|ui| {
                        if ui.add(primary("Add")).clicked() {
                            if let Some(m) = self.models.get(self.new_model).map(|m| m.0.clone()) {
                                match sim::create_device(&m, &self.new_name) {
                                    Ok(p) => {
                                        self.devices = sim::devices();
                                        self.device = self.devices.iter().position(|d| d.1 == p).unwrap_or(self.device);
                                        self.message = format!("Added {m}");
                                        self.adding = false;
                                        self.new_name.clear();
                                    }
                                    Err(e) => self.message = format!("Couldn't add it: {e:#}"),
                                }
                            }
                        }
                        if ui.add(secondary("Cancel")).clicked() {
                            self.adding = false;
                        }
                    });
                    ui.end_row();
                }
            });
            ui.add_space(10.0);
            ui.horizontal(|ui| {
                let dev = self.device_dir();
                let rt = self.runtimes.get(self.runtime).map(|r| r.1.clone());
                if ui.add_enabled(!booted && dev.is_some(), primary("▶  Start")).clicked() {
                    if let (Some(d), Some(r)) = (dev.clone(), rt.clone()) {
                        self.message = match sim::start(&d, &r, &self.jc) {
                            Ok(_) => "Starting…".to_string(),
                            Err(e) => format!("{e:#}"),
                        };
                    }
                }
                if ui.add_enabled(booted, secondary("■  Stop")).clicked() {
                    if let Some(d) = dev.clone() {
                        std::thread::spawn(move || {
                            let _ = sim::stop(&d);
                        });
                        self.message = "Stopping".into();
                    }
                }
                if !self.adding && ui.add(secondary("＋  Add phone")).clicked() {
                    self.adding = true;
                }
                if ui.add(secondary("🖵  Show")).clicked() {
                    self.message = match sim::show() {
                        Ok(()) => "Window opened".into(),
                        Err(e) => format!("{e:#}"),
                    };
                }
                if !self.message.is_empty() {
                    ui.add_space(8.0);
                    ui.label(muted(&self.message));
                }
            });
        });
        ui.add_space(14.0);
        match simst {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(muted("Checking…"));
                });
            }
            Some(s) => {
                let (st, col) = if s.home { ("Running", GREEN) } else if s.booted { ("Starting", ORANGE) } else { ("Stopped", MUTED) };
                stat_row(ui, vec![
                    ("STATUS", st.into(), col),
                    ("MEMORY", if s.booted { format!("{:.0} MB", s.pss_mb) } else { "–".into() }, TEXT),
                    ("CPU", if s.booted { format!("{:.0}%", s.cpu_pct) } else { "–".into() }, TEXT),
                    ("PROCESSES", if s.booted { s.procs.to_string() } else { "–".into() }, TEXT),
                ]);
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    if s.booted {
                                            }
                    if let Some(d) = s.device.as_ref().and_then(|d| self.devices.iter().find(|x| &x.1 == d)) {
                        ui.label(muted(format!("On: {}", d.0)).size(12.5));
                        ui.add_space(10.0);
                    }
                    ui.label(muted(if s.simview { "Window open" } else { "Window closed" }).size(12.5));
                });
            }
        }
    }

    fn page_features(&mut self, ui: &mut egui::Ui, booted: bool) {
        page_header(ui, "Features", "Turn off what you don't use to save memory.");
        let Some(dev) = self.device_dir() else {
            card(ui, |ui| ui.label(muted("Pick a phone first.")));
            return;
        };
        let prof = features::load(&dev);
        let cur = features::preset_of(&prof);
        let presets = [("full", "Full"), ("lean", "Lean"), ("minimal", "Minimal")];
        ui.horizontal(|ui| {
            ui.label(muted("Preset"));
            let sel = presets.iter().position(|p| cur.as_deref() == Some(p.0));
            if let Some(i) = segmented(ui, &presets.map(|p| p.1), sel, true) {
                let _ = features::apply_preset(&dev, presets[i].0);
                self.features_changed = true;
            }
            if cur.is_none() {
                pill(ui, "Custom", ACCENT);
            }
        });
        ui.add_space(10.0);
        let mut total_off = 0u64;
        card(ui, |ui| {
            let list = features::list(&dev);
            let n = list.len();
            for (i, f) in list.into_iter().enumerate() {
                let mut on = f.enabled;
                // "What it is. Off: what you lose." -> description, plus the consequence once it is off
                let (what, off) = match f.desc.split_once(" Off:") {
                    Some((a, b)) => (a.trim().to_string(), b.trim().to_string()),
                    None => (f.desc.clone(), String::new()),
                };
                let right_w = 150.0;
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 0.0;
                    let left_w = (ui.available_width() - right_w - 16.0).max(200.0);
                    ui.allocate_ui_with_layout(egui::vec2(left_w, 0.0), egui::Layout::top_down(egui::Align::Min), |ui| {
                        ui.set_width(left_w);
                        ui.spacing_mut().item_spacing.y = 3.0;
                        ui.label(egui::RichText::new(&f.title).size(15.0).strong());
                        if !what.is_empty() {
                            ui.add(egui::Label::new(muted(&what).size(12.5)).wrap());
                        }
                        if !on && !off.is_empty() {
                            ui.add(egui::Label::new(egui::RichText::new(off.clone()).color(ORANGE).size(12.5)).wrap());
                        }
                    });
                    ui.add_space(16.0);
                    ui.allocate_ui_with_layout(egui::vec2(right_w, 44.0), egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if toggle(ui, &mut on).changed() {
                            let _ = features::set(&dev, &f.id, on);
                            self.features_changed = true;
                        }
                        ui.add_space(14.0);
                        ui.vertical(|ui| {
                            ui.spacing_mut().item_spacing.y = 0.0;
                            ui.with_layout(egui::Layout::top_down(egui::Align::Max), |ui| {
                                ui.label(egui::RichText::new(if f.cost_mb > 0 { format!("{} MB", f.cost_mb) } else { "—".into() }).size(13.5).strong());
                                ui.label(muted(if f.when == "boot" { "every boot" } else { "when used" }).size(11.5));
                            });
                        });
                    });
                }).response.on_hover_text(&f.tech);
                if !on && f.default {
                    total_off += f.cost_mb;
                }
                if i + 1 < n {
                    ui.add_space(8.0);
                    let r = ui.available_rect_before_wrap();
                    ui.painter().hline(r.x_range(), r.top(), egui::Stroke::new(1.0_f32, BORDER));
                    ui.add_space(9.0);
                }
            }
        });
        ui.add_space(12.0);
        ui.horizontal(|ui| {
            if ui.add_enabled(booted && self.features_changed, primary("⟳  Restart now")).clicked() {
                if let Some(r) = self.runtimes.get(self.runtime).map(|r| r.1.clone()) {
                    let (d, jc) = (dev.clone(), self.jc.clone());
                    std::thread::spawn(move || {
                        let _ = sim::restart(&d, &r, &jc);
                    });
                    self.message = "Restarting…".into();
                    self.features_changed = false;
                }
            }
            if self.features_changed {
                ui.label(egui::RichText::new("Restart to apply").color(ORANGE));
            }
            if total_off > 0 {
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    pill(ui, &format!("Saving {total_off} MB"), GREEN);
                });
            }
        });
    }

    fn page_aot(&mut self, ui: &mut egui::Ui, building: bool, status: &Option<Result<aot::Status, String>>, status_busy: bool) {
        page_header(ui, "Pre-translate", "Get apps ready ahead of time so they open fast.");
        egui::Frame::new().fill(ACCENT.gamma_multiply(0.10)).stroke(egui::Stroke::new(1.0_f32, ACCENT.gamma_multiply(0.35))).corner_radius(12).inner_margin(16).show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.label(egui::RichText::new("ℹ  Takes a while the first time").strong());
            ui.add(egui::Label::new(muted(
                "The first run is slow, but you only do it once. After that, only what changed gets updated.",
            )).wrap());
        });
        ui.add_space(12.0);

        let st_ok = status.as_ref().and_then(|s| s.as_ref().ok()).cloned();
        let need = st_ok.as_ref().map(|s| s.stale + s.missing).unwrap_or(0);
        card(ui, |ui| {
            ui.horizontal(|ui| {
                if building {
                    if ui.add(egui::Button::new(egui::RichText::new("✖  Cancel").color(egui::Color32::WHITE).strong()).fill(RED.gamma_multiply(0.8)).min_size(egui::vec2(120.0, 36.0))).clicked() {
                        self.cancel_build();
                    }
                } else {
                    let resumable = self.progress.as_ref().is_some_and(|p| p.state == "cancelled" || p.state == "error");
                    let label = if resumable && need > 0 {
                        "⏵  Continue".to_string()
                    } else if need > 0 && st_ok.as_ref().is_some_and(|s| s.up_to_date > 0) {
                        "⟳  Update".to_string()
                    } else if need == 0 && st_ok.is_some() {
                        "✔  Ready".to_string()
                    } else {
                        "⚡  Start".to_string()
                    };
                    if ui.add_enabled(need > 0 || st_ok.as_ref().is_some_and(|s| !s.image_current), primary(label)).clicked() {
                        self.start_build(false);
                    }
                    if ui.add(secondary("Redo all")).on_hover_text("Start over from scratch").clicked() {
                        self.start_build(true);
                    }
                }
            });
            ui.add_space(12.0);
            egui::Grid::new("aotopts").num_columns(2).spacing([16.0, 12.0]).show(ui, |ui| {
                ui.label(muted("How much"));
                let scopes = [("hot", "Common"), ("loaded", "More"), ("all", "Everything")];
                let sel = scopes.iter().position(|s| self.cfg.scope == s.0);
                if let Some(i) = segmented(ui, &scopes.map(|s| s.1), sel, !building) {
                    self.cfg.scope = scopes[i].0.into();
                    let _ = self.paths.save_config(&self.effective_cfg());
                    self.refresh_status();
                }
                ui.end_row();
                ui.label(muted("Speed"));
                ui.horizontal(|ui| {
                    ui.add_enabled(!building, egui::Slider::new(&mut self.cores, 1..=self.max_cores));
                    ui.label(muted("cores, won't slow your PC").size(12.5));
                });
                ui.end_row();
                ui.label(muted("Skip off features"));
                ui.horizontal(|ui| {
                    let mut f = self.cfg.follow_features;
                    if ui.add_enabled_ui(!building, |ui| toggle(ui, &mut f)).inner.changed() {
                        self.cfg.follow_features = f;
                        let _ = self.paths.save_config(&self.effective_cfg());
                        self.refresh_status();
                    }
                    ui.label(muted("skip what you turned off").size(12.5));
                });
                ui.end_row();
            });

            if let Some(p) = &self.progress {
                let live = building || p.state == "building" && aot_alive(p.pid);
                if live || matches!(p.state.as_str(), "done" | "cancelled" | "error") {
                    ui.add_space(14.0);
                    ui.separator();
                    ui.add_space(10.0);
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
        ui.add_space(14.0);

        ui.horizontal(|ui| {
            ui.label(egui::RichText::new("Status").size(18.0).strong());
            if ui.add_enabled(!status_busy, egui::Button::new("↻").min_size(egui::vec2(32.0, 28.0))).on_hover_text("Check again").clicked() {
                self.refresh_status();
            }
            if status_busy {
                ui.spinner();
            }
        });
        ui.add_space(4.0);
        match status {
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
                ui.add_space(6.0);
                if s.dropped > 0 {
                    ui.label(muted(format!("{} more get ready the first time they're used.", s.dropped)).size(12.5));
                }
                ui.add_space(10.0);
                card(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.add(egui::TextEdit::singleline(&mut self.filter).hint_text("🔍  Search").desired_width(260.0));
                        ui.add_space(10.0);
                        toggle(ui, &mut self.only_attention);
                        ui.label(muted("Only show what needs work"));
                    });
                    ui.add_space(8.0);
                    egui::Grid::new("chunks").striped(true).num_columns(4).spacing([18.0, 6.0]).min_col_width(60.0).show(ui, |ui| {
                        for h in ["STATUS", "NAME", "SIZE", "NOTE"] {
                            ui.label(muted(h).size(11.5).strong());
                        }
                        ui.end_row();
                        let mut shown = 0;
                        let f = self.filter.to_lowercase();
                        for c in &s.chunks {
                            if self.only_attention && c.state == "up-to-date" {
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
                            ui.label(muted(if self.only_attention { "All done" } else { "Nothing matches" }));
                            ui.end_row();
                        }
                    });
                });
            }
        }
    }
}

fn aot_alive(pid: u32) -> bool {
    pid != 0 && std::path::Path::new(&format!("/proc/{pid}")).exists()
}
