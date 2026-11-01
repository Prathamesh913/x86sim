//! Sim control for the arm64 device: start / stop (cli/libexec/arm64/boot-a64.sh), show (simview on DISPLAY=:0) and
//! status (booted, home reached, CPU, PSS) read from /proc. Processes are found by exact checks (the darlingserver
//! of the sim's prefix and its descendants), never by pattern kills.
use crate::aot::{repo_root, scratch};
use anyhow::{Context, Result};
use serde::Serialize;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub fn sim_prefix() -> PathBuf {
    std::env::var("A64_DPREFIX").map(PathBuf::from).unwrap_or_else(|_| scratch().join("a64prefix"))
}

pub fn runtimes() -> Vec<(String, PathBuf)> {
    let dir = repo_root().join("runtimes");
    let mut v: Vec<(String, PathBuf)> = std::fs::read_dir(&dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let rr = e.path().join("RuntimeRoot");
            crate::dyldcache::Cache::cache_dir(&rr).join(crate::dyldcache::CACHE_NAME).exists().then(|| (runtime_label(&e.file_name().to_string_lossy()), rr))
        })
        .collect();
    v.sort();
    v
}

pub fn devices() -> Vec<(String, PathBuf)> {
    let dir = repo_root().join("var");
    let mut v: Vec<(String, PathBuf)> = std::fs::read_dir(&dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("device-a64") && e.path().join("sim.env").exists())
        .map(|e| (device_label(&e.path()), e.path()))
        .collect();
    v.sort();
    v
}

/// "ios-27.0-24A434-arm64" -> "iOS 27.0 (24A434)"
fn runtime_label(dir: &str) -> String {
    match dir.split('-').collect::<Vec<_>>()[..] {
        ["ios", ver, build, ..] => format!("iOS {ver} ({build})"),
        _ => dir.to_string(),
    }
}

/// The phone models a device can be made of (cli/lib/devicetypes-a64.json, from Xcode's device types): (name, model id)
pub fn models() -> Vec<(String, String)> {
    let txt = std::fs::read_to_string(repo_root().join("cli/lib/devicetypes-a64.json")).unwrap_or_default();
    let v: serde_json::Value = serde_json::from_str(&txt).unwrap_or_default();
    let mut out = vec![];
    if let Some(m) = v.as_object() {
        for (name, t) in m {
            out.push((name.clone(), t["model"].as_str().unwrap_or("").to_string()));
        }
    }
    out.sort_by_key(|m| !m.0.starts_with("iPhone"));
    out
}

/// "iPhone 17 Pro Max · Pro Max" from the device's sim.env (model) and name file (else the folder name)
fn device_label(dir: &Path) -> String {
    let folder = dir.file_name().unwrap_or_default().to_string_lossy().into_owned();
    let name = std::fs::read_to_string(dir.join("name")).map(|s| s.trim().to_string()).unwrap_or(folder);
    let env = std::fs::read_to_string(dir.join("sim.env")).unwrap_or_default();
    let model = env.lines().find_map(|l| l.strip_prefix("SIMULATOR_MODEL_IDENTIFIER=")).unwrap_or("");
    if model.is_empty() {
        return name;
    }
    let product = models().into_iter().find(|m| m.1 == model).map(|m| m.0).unwrap_or_else(|| model.to_string());
    if product == name {
        product
    } else {
        format!("{product} · {name}")
    }
}

/// New device of a model (cli/libexec/arm64/device-a64.py create): var/device-a64-<name>
pub fn create_device(model: &str, name: &str) -> Result<PathBuf> {
    let mut cmd = Command::new("python3");
    cmd.arg(repo_root().join("cli/libexec/arm64/device-a64.py")).args(["create", "--model", model]);
    if !name.trim().is_empty() {
        cmd.arg(name.trim());
    }
    let out = cmd.current_dir(repo_root()).stdin(Stdio::null()).output()?;
    anyhow::ensure!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr).trim());
    Ok(PathBuf::from(String::from_utf8_lossy(&out.stdout).trim()))
}

/// The device the arm64 sim was last booted with (boot-a64.sh writes var/run-a64/device)
pub fn booted_device() -> Option<PathBuf> {
    let s = std::fs::read_to_string(repo_root().join("var/run-a64/device")).ok()?;
    let p = PathBuf::from(s.trim());
    p.exists().then_some(p)
}

fn boot_script() -> PathBuf {
    repo_root().join("cli/libexec/arm64/boot-a64.sh")
}

pub fn start(device: &Path, runtime: &Path, jitcache: &Path) -> Result<u32> {
    let log = std::fs::File::create(scratch().join("launcher-boot.log"))?;
    let mut cmd = Command::new("setsid");
    cmd.arg(boot_script())
        .current_dir(repo_root())
        .env("A64_KEEP", "1")
        .env("X86CODE_METAL", "all")
        .env("SECS", "20")
        .env("A64_DEVICE", device)
        .env("A64_RUNTIME", runtime)
        .env("X86CODE_A64_JITCACHE_DIR", jitcache)
        .stdin(Stdio::null())
        .stdout(log.try_clone()?)
        .stderr(log);
    Ok(cmd.spawn().context("starting boot-a64.sh")?.id())
}

pub fn stop(device: &Path) -> Result<()> {
    let st = Command::new(boot_script())
        .current_dir(repo_root())
        .env("A64_STOP_ONLY", "1")
        .env("A64_DEVICE", device)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()?;
    anyhow::ensure!(st.success(), "boot-a64.sh A64_STOP_ONLY=1 failed");
    Ok(())
}

pub fn simview_running() -> bool {
    Command::new("systemctl").args(["--user", "is-active", "--quiet", "x86sim-simview"]).status().is_ok_and(|s| s.success())
}

/// simview with the console pane on DISPLAY=:0 (a systemd user service, as the round scripts run it)
pub fn show() -> Result<()> {
    if simview_running() {
        return Ok(());
    }
    let st = Command::new("systemd-run")
        .args(["--user", "--unit", "x86sim-simview", "--collect", "-E", "DISPLAY=:0", "-E", "SIMFB_RING_LINUX=/dev/shm/x86code-a64-simfb.ring", "-p"])
        .arg(format!("WorkingDirectory={}", repo_root().display()))
        .args(["/usr/bin/python3", "cli/libexec/simview.py"])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()?;
    anyhow::ensure!(st.success(), "systemd-run simview failed");
    Ok(())
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct SimStatus {
    pub booted: bool,
    /// the booted device (None when stopped)
    pub device: Option<PathBuf>,
    pub home: bool,
    pub springboard_pid: Option<u32>,
    pub procs: usize,
    pub cpu_pct: f64,
    pub pss_mb: f64,
    pub pss_anon_mb: f64,
    pub simview: bool,
}

fn read(p: &str) -> String {
    std::fs::read_to_string(p).unwrap_or_default()
}

/// The sim's processes: the darlingserver of its prefix, all its descendants, and mtlvk.
pub fn sim_pids() -> Vec<(u32, String)> {
    let prefix = sim_prefix();
    let mut ppid: HashMap<u32, Vec<u32>> = HashMap::new();
    let mut comm: HashMap<u32, String> = HashMap::new();
    let mut root = None;
    let mut extra = vec![];
    for e in std::fs::read_dir("/proc").into_iter().flatten().flatten() {
        let Ok(pid) = e.file_name().to_string_lossy().parse::<u32>() else { continue };
        let c = read(&format!("/proc/{pid}/comm")).trim().to_string();
        let stat = read(&format!("/proc/{pid}/stat"));
        let pp = stat.rsplit(')').next().and_then(|r| r.split_whitespace().nth(1)).and_then(|x| x.parse().ok()).unwrap_or(0);
        ppid.entry(pp).or_default().push(pid);
        if c == "darlingserver" {
            let args = read(&format!("/proc/{pid}/cmdline"));
            if args.split('\0').nth(1) == Some(prefix.to_string_lossy().as_ref()) {
                root = Some(pid);
            }
        }
        if c == "mtlvk" {
            extra.push(pid);
        }
        comm.insert(pid, c);
    }
    let mut out = vec![];
    if let Some(r) = root {
        let mut work = vec![r];
        while let Some(p) = work.pop() {
            out.push((p, comm.get(&p).cloned().unwrap_or_default()));
            work.extend(ppid.get(&p).into_iter().flatten().copied());
        }
        out.extend(extra.into_iter().map(|p| (p, "mtlvk".to_string())));
    }
    out
}

fn cpu_ticks(pid: u32) -> u64 {
    let s = read(&format!("/proc/{pid}/stat"));
    let f: Vec<&str> = s.rsplit(')').next().unwrap_or("").split_whitespace().collect();
    f.get(11).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0) + f.get(12).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0)
}

/// Sim status; samples CPU over `sample_ms`.
pub fn status(sample_ms: u64) -> SimStatus {
    let pids = sim_pids();
    let mut st = SimStatus { simview: simview_running(), ..Default::default() };
    if pids.is_empty() {
        return st;
    }
    st.booted = pids.iter().any(|(_, c)| c == "launchd_sim");
    st.springboard_pid = pids.iter().find(|(_, c)| c == "SpringBoard").map(|p| p.0);
    st.procs = pids.len();
    let t0: u64 = pids.iter().map(|p| cpu_ticks(p.0)).sum();
    std::thread::sleep(std::time::Duration::from_millis(sample_ms));
    let t1: u64 = pids.iter().map(|p| cpu_ticks(p.0)).sum();
    let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) }.max(1) as f64;
    st.cpu_pct = (t1.saturating_sub(t0)) as f64 / hz / (sample_ms as f64 / 1000.0) * 100.0;
    for (p, _) in &pids {
        for l in read(&format!("/proc/{p}/smaps_rollup")).lines() {
            let mut it = l.split_whitespace();
            match (it.next(), it.next().and_then(|v| v.parse::<f64>().ok())) {
                (Some("Pss:"), Some(v)) => st.pss_mb += v / 1024.0,
                (Some("Pss_Anon:"), Some(v)) => st.pss_anon_mb += v / 1024.0,
                _ => {}
            }
        }
    }
    // Home once seen stays true for that SpringBoard: an open app (a dark one fails the frame test) is still running.
    static HOME_SB: std::sync::Mutex<Option<u32>> = std::sync::Mutex::new(None);
    let mut seen = HOME_SB.lock().unwrap();
    st.home = match st.springboard_pid {
        Some(sb) if *seen == Some(sb) => true,
        Some(sb) if home_frame() => {
            *seen = Some(sb);
            true
        }
        _ => false,
    };
    if st.booted {
        st.device = booted_device();
    }
    st
}

/// framecheck's home test on the arm64 frame ring
pub fn home_frame() -> bool {
    Command::new("python3")
        .arg(repo_root().join("cli/libexec/framecheck.py"))
        .arg("/dev/shm/x86code-a64-simfb.ring")
        .arg("home")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

pub fn restart(device: &Path, runtime: &Path, jitcache: &Path) -> Result<u32> {
    stop(device)?;
    start(device, runtime, jitcache)
}
