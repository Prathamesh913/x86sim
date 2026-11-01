//! Host side: iOS versions (downloaded / in Apple's catalogue, fetched with cli/x86sim runtime), disk use of the
//! scratch dir and the caches, hub-wide settings.
use crate::aot::{repo_root, scratch};
use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[derive(Debug, Clone, Serialize)]
pub struct Runtime {
    /// runtimes/ dir name
    pub dir: String,
    pub version: String,
    pub build: String,
    pub root: PathBuf,
    /// dyld cache on disk: boots
    pub present: bool,
    /// a dir exists but is incomplete (data loss / interrupted download)
    pub partial: bool,
    pub jc: PathBuf,
}

pub fn runtimes() -> Vec<Runtime> {
    let mut v: Vec<Runtime> = std::fs::read_dir(repo_root().join("runtimes"))
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let dir = e.file_name().to_string_lossy().into_owned();
            let parts: Vec<&str> = dir.split('-').collect();
            if parts.len() < 3 || parts[0] != "ios" {
                return None;
            }
            let root = e.path().join("RuntimeRoot");
            let present = crate::dyldcache::Cache::cache_dir(&root).join(crate::dyldcache::CACHE_NAME).exists();
            Some(Runtime { jc: crate::sim::jc_for(&root), version: parts[1].to_string(), build: parts[2].to_string(), dir, root, present, partial: !present })
        })
        .collect();
    v.sort_by(|a, b| a.version.cmp(&b.version));
    v
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Catalogue {
    pub version: String,
    pub build: String,
    pub name: String,
    pub gb: f64,
}

/// Apple's catalogue (network; `x86sim runtime list`), cached in scratch
pub fn catalogue(refresh: bool) -> Vec<Catalogue> {
    let cache = scratch().join("launcher-runtimes.json");
    if !refresh {
        if let Some(v) = std::fs::read(&cache).ok().and_then(|b| serde_json::from_slice(&b).ok()) {
            return v;
        }
    }
    let out = Command::new(repo_root().join("cli/x86sim")).args(["runtime", "list"]).stdin(Stdio::null()).output();
    let mut v = vec![];
    if let Ok(o) = out {
        for l in String::from_utf8_lossy(&o.stdout).lines() {
            // "{build:12} {name:42} {size:4.1f} GB{mark}"
            let f: Vec<&str> = l.split_whitespace().collect();
            if f.len() < 4 {
                continue;
            }
            let build = f[0].to_string();
            let gb_i = f.iter().position(|x| *x == "GB").unwrap_or(f.len());
            let gb = gb_i.checked_sub(1).and_then(|i| f.get(i)).and_then(|x| x.parse().ok()).unwrap_or(0.0);
            let name = f[1..gb_i.saturating_sub(1)].join(" ");
            let version = name.split_whitespace().find(|w| w.starts_with("27")).unwrap_or("").to_string();
            v.push(Catalogue { version, build, name, gb });
        }
    }
    if !v.is_empty() {
        let _ = crate::aot::write_json(&cache, &v);
    }
    v
}

pub fn fetch_log(build: &str) -> PathBuf {
    scratch().join(format!("launcher-fetch-{build}.log"))
}

/// Downloads a runtime (x86sim runtime fetch BUILD) in the background; progress in fetch_log
pub fn fetch(build: &str) -> Result<u32> {
    let log = std::fs::File::create(fetch_log(build))?;
    let c = Command::new("setsid").arg(repo_root().join("cli/x86sim")).args(["runtime", "fetch", build]).current_dir(repo_root()).stdin(Stdio::null()).stdout(log.try_clone()?).stderr(log).spawn()?;
    Ok(c.id())
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct Disk {
    pub total_gb: f64,
    pub free_gb: f64,
}

pub fn disk(p: &Path) -> Disk {
    let c = std::ffi::CString::new(p.to_string_lossy().as_bytes()).unwrap_or_default();
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut st) } != 0 {
        return Disk::default();
    }
    let bs = st.f_frsize as f64;
    Disk { total_gb: st.f_blocks as f64 * bs / 1e9, free_gb: st.f_bavail as f64 * bs / 1e9 }
}

/// Bytes under a dir (walk; symlinks not followed)
pub fn dir_size(p: &Path) -> u64 {
    let mut total = 0;
    let mut work = vec![p.to_path_buf()];
    while let Some(d) = work.pop() {
        for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            let Ok(md) = e.metadata() else { continue };
            if md.is_dir() {
                work.push(e.path());
            } else if md.is_file() {
                total += md.len();
            }
        }
    }
    total
}

pub fn mtlvk_pids() -> Vec<u32> {
    std::fs::read_dir("/proc")
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| e.file_name().to_string_lossy().parse::<u32>().ok())
        .filter(|p| std::fs::read_to_string(format!("/proc/{p}/comm")).map(|c| c.trim() == "mtlvk").unwrap_or(false))
        .collect()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct HubSettings {
    /// show live thumbnails on the home view
    pub thumbnails: bool,
    /// open the phone's view when it starts
    pub open_on_start: bool,
    /// home view card width
    pub card_w: f32,
}

impl Default for HubSettings {
    fn default() -> Self {
        HubSettings { thumbnails: true, open_on_start: false, card_w: 250.0 }
    }
}

pub fn hub_path() -> PathBuf {
    scratch().join("launcher-hub.json")
}

pub fn load_hub() -> HubSettings {
    std::fs::read(hub_path()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

pub fn save_hub(s: &HubSettings) -> Result<()> {
    crate::aot::write_json(&hub_path(), s)
}
