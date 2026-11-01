//! Input to a phone: host/simhid's socket (<device>/data/simhid.sock), one text command per line (touch, touch2,
//! key, button). Edge touches are marked as Apple's Simulator does (simview.py's rule: within 4% of the width of an
//! edge; top and bottom win in the corners), so the home indicator and Control/Notification Center work.
use std::io::Write;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub struct Hid {
    path: PathBuf,
    sock: Option<UnixStream>,
}

impl Hid {
    pub fn new(path: &Path) -> Hid {
        Hid { path: path.to_path_buf(), sock: None }
    }

    pub fn send(&mut self, line: &str) {
        static DEBUG: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *DEBUG.get_or_init(|| std::env::var_os("X86CODE_HID_DEBUG").is_some()) {
            eprintln!("hid {}: {line}", self.path.display());
        }
        for _ in 0..2 {
            if self.sock.is_none() {
                match UnixStream::connect(&self.path) {
                    Ok(s) => {
                        let _ = s.set_write_timeout(Some(Duration::from_millis(500)));
                        self.sock = Some(s);
                    }
                    Err(_) => return,
                }
            }
            if self.sock.as_mut().unwrap().write_all(format!("{line}\n").as_bytes()).is_ok() {
                return;
            }
            self.sock = None;
        }
    }

    /// x, y normalized 0..1; screen (w, h) in device pixels for the edge rule
    pub fn touch(&mut self, phase: &str, x: f32, y: f32, edge: Option<&str>) {
        let (x, y) = (x.clamp(0.0, 1.0), y.clamp(0.0, 1.0));
        match edge {
            Some(e) => self.send(&format!("touch {phase} {x:.5} {y:.5} {e}")),
            None => self.send(&format!("touch {phase} {x:.5} {y:.5}")),
        }
    }

    pub fn touch2(&mut self, phase: &str, x1: f32, y1: f32, x2: f32, y2: f32) {
        self.send(&format!("touch2 {phase} {:.5} {:.5} {:.5} {:.5}", x1.clamp(0.0, 1.0), y1.clamp(0.0, 1.0), x2.clamp(0.0, 1.0), y2.clamp(0.0, 1.0)));
    }

    pub fn key(&mut self, down: bool, usage: u32) {
        self.send(&format!("key {} {usage}", if down { "down" } else { "up" }));
    }

    pub fn button(&mut self, name: &str, down: bool) {
        self.send(&format!("button {name} {}", if down { "down" } else { "up" }));
    }

    pub fn press(&mut self, name: &str) {
        self.button(name, true);
        std::thread::sleep(Duration::from_millis(80));
        self.button(name, false);
    }
}

/// The edge a touch at normalized (x, y) begins on, for a screen of w x h device pixels.
pub fn edge_at(x: f32, y: f32, w: u32, h: u32) -> Option<&'static str> {
    let mx = 0.04;
    let my = 0.04 * w as f32 / h.max(1) as f32;
    if y >= 1.0 - my {
        Some("bottom")
    } else if y <= my {
        Some("top")
    } else if x <= mx {
        Some("left")
    } else if x >= 1.0 - mx {
        Some("right")
    } else {
        None
    }
}
