//! The embedded screen of a phone: a thread per phone sleeps on its frame ring (futex) and converts new frames to
//! RGBA at the size the view needs (full for the open phone, a few pixels wide for a home-view thumbnail); the GUI
//! uploads them as a texture. Mouse on the screen = touch (edge touches marked as simview does), keyboard = the
//! phone's hardware keyboard, while the screen has focus.
use crate::theme::*;
use eframe::egui;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use x86code_core::{hid, instance::Instance, ring::Ring};

pub const OFF: u8 = 0;
pub const THUMB: u8 = 1;
pub const FULL: u8 = 2;

pub struct Img {
    pub w: usize,
    pub h: usize,
    pub pixels: Vec<egui::Color32>,
    pub seq: u64,
}

#[derive(Default)]
pub struct Shared {
    pub mode: AtomicU8,
    pub target_w: AtomicU32,
    /// unix seconds of this boot: older frames are a past session's (f64 bits)
    pub boot_time: AtomicU64,
    pub frame: Mutex<Option<Img>>,
    pub dirty: AtomicBool,
    pub seq: AtomicU64,
    pub fps: Mutex<Vec<std::time::Instant>>,
    /// the frame ring this phone publishes to: a boot rewrites sim.env, so the path can change while the viewer lives
    pub ring: Mutex<std::path::PathBuf>,
}

pub struct Viewer {
    pub shared: Arc<Shared>,
    tex: Option<egui::TextureHandle>,
    tex_seq: u64,
    hid: hid::Hid,
    touching: bool,
    last_norm: (f32, f32),
    pub focused: bool,
    pub screen: (u32, u32),
    pub id: String,
    pinch: bool,
    pan2: bool,
}

fn convert(f: &x86code_core::ring::Frame, step: usize) -> Img {
    let (w, h) = (f.w as usize / step, f.h as usize / step);
    let mut px = Vec::with_capacity(w * h);
    for y in 0..h {
        let row = y * step * f.stride as usize;
        for x in 0..w {
            let o = row + x * step * 4;
            px.push(egui::Color32::from_rgb(f.bgra[o + 2], f.bgra[o + 1], f.bgra[o]));
        }
    }
    Img { w, h, pixels: px, seq: f.seq }
}

impl Viewer {
    pub fn new(inst: &Instance, ctx: egui::Context) -> Viewer {
        let shared = Arc::new(Shared::default());
        shared.target_w.store(inst.screen.0, Ordering::Relaxed);
        *shared.ring.lock().unwrap() = inst.ring.clone();
        let sh = shared.clone();
        std::thread::Builder::new()
            .name(format!("ring {}", inst.id))
            .spawn(move || {
                let mut ring_path = sh.ring.lock().unwrap().clone();
                let mut ring = Ring::new(&ring_path);
                let mut last = (0u64, 0usize);
                loop {
                    let want = sh.ring.lock().unwrap().clone();
                    if want != ring_path {
                        ring = Ring::new(&want);
                        ring_path = want;
                        last = (0, 0);
                    }
                    let mode = sh.mode.load(Ordering::Relaxed);
                    if mode == OFF {
                        std::thread::sleep(Duration::from_millis(250));
                        continue;
                    }
                    let seq = ring.seq();
                    let tw = sh.target_w.load(Ordering::Relaxed).max(8) as usize;
                    if seq != 0 {
                        if let Some(f) = ring.latest() {
                            let step = ((f.w as usize + tw - 1) / tw).max(1);
                            if (f.seq, step) != last {
                                let boot = f64::from_bits(sh.boot_time.load(Ordering::Relaxed));
                                if (f.t_ns as f64) / 1e9 >= boot - 2.0 {
                                    let img = convert(&f, step);
                                    *sh.frame.lock().unwrap() = Some(img);
                                    sh.dirty.store(true, Ordering::Release);
                                    sh.seq.store(f.seq, Ordering::Relaxed);
                                    let now = std::time::Instant::now();
                                    let mut fps = sh.fps.lock().unwrap();
                                    fps.retain(|t| now.duration_since(*t) < Duration::from_secs(1));
                                    fps.push(now);
                                    ctx.request_repaint();
                                }
                                last = (f.seq, step);
                            }
                        }
                    }
                    if mode == THUMB || seq == 0 {
                        std::thread::sleep(Duration::from_millis(if seq == 0 { 1000 } else { 500 }));
                    } else {
                        ring.wait(seq, Duration::from_secs(1));
                    }
                }
            })
            .expect("ring thread");
        Viewer { shared, tex: None, tex_seq: 0, hid: hid::Hid::new(&inst.sock), touching: false, last_norm: (0.0, 0.0), focused: false, screen: inst.screen, id: inst.id.clone(), pinch: false, pan2: false }
    }

    /// Follow the phone's current ring (sim.env after its last boot); a change drops the old ring's picture.
    pub fn set_ring(&mut self, p: &std::path::Path) {
        let mut r = self.shared.ring.lock().unwrap();
        if r.as_path() != p {
            *r = p.to_path_buf();
            drop(r);
            *self.shared.frame.lock().unwrap() = None;
            self.clear();
        }
    }

    pub fn set_boot_time(&self, t: Option<f64>) {
        self.shared.boot_time.store(t.unwrap_or(f64::MAX).to_bits(), Ordering::Relaxed);
    }

    pub fn fps(&self) -> usize {
        let now = std::time::Instant::now();
        self.shared.fps.lock().unwrap().iter().filter(|t| now.duration_since(**t) < Duration::from_secs(1)).count()
    }

    fn upload(&mut self, ctx: &egui::Context) {
        if !self.shared.dirty.swap(false, Ordering::AcqRel) {
            return;
        }
        let img = self.shared.frame.lock().unwrap().take();
        if let Some(img) = img {
            let ci = egui::ColorImage { size: [img.w, img.h], source_size: egui::vec2(img.w as f32, img.h as f32), pixels: img.pixels };
            let opts = egui::TextureOptions::LINEAR;
            match &mut self.tex {
                Some(t) => t.set(ci, opts),
                None => self.tex = Some(ctx.load_texture(format!("screen-{}", self.id), ci, opts)),
            }
            self.tex_seq = img.seq;
        }
    }

    pub fn clear(&mut self) {
        self.tex = None;
        self.tex_seq = 0;
    }

    /// Draws the screen into `avail` (keeping the phone's aspect), with a bezel. `live`: frames count as the screen.
    /// `interactive`: mouse and keyboard go to the phone. Returns the screen rect.
    pub fn show(&mut self, ui: &mut egui::Ui, avail: egui::Vec2, live: bool, placeholder: &str, interactive: bool, mode: u8) -> egui::Rect {
        self.shared.mode.store(if live { mode } else { OFF }, Ordering::Relaxed);
        if !live {
            self.clear();
        } else {
            self.upload(ui.ctx());
        }
        let (sw, sh) = (self.screen.0 as f32, self.screen.1 as f32);
        let bezel = if mode == FULL { 10.0 } else { 5.0 };
        let inner = avail - egui::vec2(2.0 * bezel, 2.0 * bezel);
        let scale = (inner.x / sw).min(inner.y / sh).max(0.01);
        let size = egui::vec2(sw * scale, sh * scale);
        self.shared.target_w.store(size.x.ceil() as u32 * 1, Ordering::Relaxed);
        let outer = size + egui::vec2(2.0 * bezel, 2.0 * bezel);
        let (rect, resp) = ui.allocate_exact_size(outer, if interactive { egui::Sense::click_and_drag() } else { egui::Sense::click() });
        let screen = rect.shrink(bezel);
        let p = ui.painter();
        let radius = (size.x * 0.11).max(4.0);
        p.rect_filled(rect, radius + bezel * 0.7, BEZEL);
        p.rect_stroke(rect, radius + bezel * 0.7, egui::Stroke::new(1.0_f32, BORDER_STRONG), egui::StrokeKind::Inside);
        match (&self.tex, live) {
            (Some(t), true) => {
                egui::Image::new(egui::load::SizedTexture::new(t.id(), size)).corner_radius(radius).paint_at(ui, screen);
            }
            _ => {
                p.rect_filled(screen, radius, egui::Color32::from_rgb(0x0c, 0x0c, 0x0e));
                if !placeholder.is_empty() {
                    let fs = if mode == FULL { T_LG } else { T_SMALL };
                    let ic = if placeholder == "Off" { icon::POWER } else { icon::CIRCLE_NOTCH };
                    p.text(screen.center() - egui::vec2(0.0, fs * 0.9), egui::Align2::CENTER_CENTER, ic, font(fs * 1.6), INK_3);
                    p.text(screen.center() + egui::vec2(0.0, fs * 0.9), egui::Align2::CENTER_CENTER, placeholder, font_m(fs), INK_3);
                }
            }
        }
        if interactive {
            self.input(ui, &resp, screen, live);
            if self.focused {
                ui.painter().rect_stroke(rect, radius + bezel * 0.7, egui::Stroke::new(1.5_f32, ACCENT), egui::StrokeKind::Outside);
            }
        }
        rect
    }

    fn input(&mut self, ui: &mut egui::Ui, resp: &egui::Response, screen: egui::Rect, live: bool) {
        let ctx = ui.ctx().clone();
        let (pressed, released, pos, moving, shift, ctrl) = ctx.input(|i| (i.pointer.primary_pressed(), i.pointer.primary_released(), i.pointer.interact_pos().or(i.pointer.latest_pos()), i.pointer.is_moving(), i.modifiers.shift, i.modifiers.ctrl));
        let norm = |p: egui::Pos2| ((p.x - screen.left()) / screen.width(), (p.y - screen.top()) / screen.height());
        if pressed {
            if let Some(p) = pos {
                if resp.rect.contains(p) {
                    self.focused = true;
                    ui.memory_mut(|m| m.request_focus(resp.id));
                    if live {
                        let (x, y) = norm(p);
                        self.last_norm = (x, y);
                        self.touching = true;
                        if ctrl || shift {
                            self.two_finger("down", x, y, ctrl);
                        } else {
                            let edge = hid::edge_at(x, y, self.screen.0, self.screen.1);
                            self.hid.touch("down", x, y, edge);
                        }
                        self.pinch = ctrl;
                        self.pan2 = shift;
                    }
                } else {
                    self.focused = false;
                }
            }
        }
        if self.touching && moving {
            if let Some(p) = pos {
                let (x, y) = norm(p);
                let (x, y) = (x.clamp(0.0, 1.0), y.clamp(0.0, 1.0));
                if (x, y) != self.last_norm {
                    self.last_norm = (x, y);
                    if self.pinch || self.pan2 {
                        self.two_finger("move", x, y, self.pinch);
                    } else {
                        self.hid.touch("move", x, y, None);
                    }
                }
            }
        }
        if self.touching && released {
            self.touching = false;
            let (x, y) = self.last_norm;
            if self.pinch || self.pan2 {
                self.two_finger("up", x, y, self.pinch);
            } else {
                self.hid.touch("up", x, y, None);
            }
            self.pinch = false;
            self.pan2 = false;
        }
        if self.focused && live {
            if resp.hovered() {
                ui.output_mut(|o| o.cursor_icon = egui::CursorIcon::PointingHand);
            }
            let mut sends: Vec<(bool, u32, bool)> = vec![]; // (down, usage, shifted)
            let mut buttons: Vec<(&'static str, bool)> = vec![];
            ctx.input_mut(|i| {
                i.events.retain(|e| match e {
                    egui::Event::Key { key, pressed, repeat, modifiers, .. } => {
                        if *repeat {
                            return false;
                        }
                        match *key {
                            egui::Key::F1 => buttons.push(("home", *pressed)),
                            egui::Key::F2 => buttons.push(("lock", *pressed)),
                            egui::Key::F3 => buttons.push(("volup", *pressed)),
                            egui::Key::F4 => buttons.push(("voldown", *pressed)),
                            egui::Key::H if modifiers.ctrl => buttons.push(("home", *pressed)),
                            k => {
                                if let Some((u, sh)) = usage(k) {
                                    sends.push((*pressed, u, sh || modifiers.shift));
                                }
                            }
                        }
                        false
                    }
                    egui::Event::Text(_) | egui::Event::Paste(_) | egui::Event::Copy | egui::Event::Cut => false,
                    _ => true,
                });
            });
            for (b, down) in buttons {
                self.hid.button(b, down);
            }
            for (down, u, sh) in sends {
                if sh && down {
                    self.hid.key(true, 225);
                }
                self.hid.key(down, u);
                if sh && !down {
                    self.hid.key(false, 225);
                }
            }
        }
    }

    fn two_finger(&mut self, phase: &str, x: f32, y: f32, pinch: bool) {
        if pinch {
            self.hid.touch2(phase, x, y, 1.0 - x, 1.0 - y);
        } else {
            let dx = 50.0 / self.screen.0 as f32;
            self.hid.touch2(phase, x - dx, y, x + dx, y);
        }
    }

    pub fn press(&mut self, button: &str) {
        self.hid.press(button);
    }
}

/// egui key -> (HID usage, needs shift)
fn usage(k: egui::Key) -> Option<(u32, bool)> {
    use egui::Key as K;
    let n = k.name();
    if n.len() == 1 {
        let c = n.chars().next().unwrap();
        if c.is_ascii_alphabetic() {
            return Some((4 + (c.to_ascii_lowercase() as u32 - 'a' as u32), false));
        }
    }
    Some(match k {
        K::Num0 => (0x27, false),
        K::Num1 => (0x1e, false),
        K::Num2 => (0x1f, false),
        K::Num3 => (0x20, false),
        K::Num4 => (0x21, false),
        K::Num5 => (0x22, false),
        K::Num6 => (0x23, false),
        K::Num7 => (0x24, false),
        K::Num8 => (0x25, false),
        K::Num9 => (0x26, false),
        K::Space => (0x2c, false),
        K::Enter => (0x28, false),
        K::Escape => (0x29, false),
        K::Backspace => (0x2a, false),
        K::Tab => (0x2b, false),
        K::Minus => (0x2d, false),
        K::Equals => (0x2e, false),
        K::Plus => (0x2e, true),
        K::OpenBracket => (0x2f, false),
        K::CloseBracket => (0x30, false),
        K::OpenCurlyBracket => (0x2f, true),
        K::CloseCurlyBracket => (0x30, true),
        K::Backslash => (0x31, false),
        K::Pipe => (0x31, true),
        K::Semicolon => (0x33, false),
        K::Colon => (0x33, true),
        K::Quote => (0x34, false),
        K::Backtick => (0x35, false),
        K::Comma => (0x36, false),
        K::Period => (0x37, false),
        K::Slash => (0x38, false),
        K::Questionmark => (0x38, true),
        K::Exclamationmark => (0x1e, true),
        K::ArrowRight => (0x4f, false),
        K::ArrowLeft => (0x50, false),
        K::ArrowDown => (0x51, false),
        K::ArrowUp => (0x52, false),
        K::Delete => (0x4c, false),
        K::Home => (0x4a, false),
        K::End => (0x4d, false),
        K::PageUp => (0x4b, false),
        K::PageDown => (0x4e, false),
        _ => return None,
    })
}
