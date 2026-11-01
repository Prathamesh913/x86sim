//! Instances: every var/device-a64* dir is a phone with its own Darling prefix, state dir and frame ring, so several
//! can run side by side. The default phone (var/device-a64) keeps the default dirs (a64prefix, var/run-a64, the
//! default ring: the standalone simview and the round scripts keep working); device-a64<sfx> gets a64prefix-<sfx>,
//! run-a64<sfx> and its own ring. A running instance is found by facts, never by patterns: its launchd_sim's
//! command line names the device dir; its tree, darlingserver ancestor and mtlvk make up the instance's processes.
use crate::aot::{repo_root, scratch};
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[derive(Debug, Clone, Serialize)]
pub struct Instance {
    /// folder name (device-a64-ios272)
    pub id: String,
    pub dir: PathBuf,
    /// the name given at creation (else the folder)
    pub name: String,
    /// "iPhone 17 Pro"
    pub model: String,
    pub model_id: String,
    /// runtimes/ dir name
    pub runtime_dir: String,
    pub runtime_root: PathBuf,
    /// the runtime's dyld cache is on disk (a 0-byte RuntimeRoot after a data loss is not)
    pub runtime_present: bool,
    /// "iOS 27.2"
    pub ios: String,
    pub build: String,
    pub screen: (u32, u32),
    pub scale: f32,
    pub ring: PathBuf,
    pub sock: PathBuf,
    pub state_dir: PathBuf,
    pub prefix: PathBuf,
    pub log_dir: PathBuf,
    pub is_default: bool,
    pub udid: String,
}

fn env_of(dir: &Path) -> HashMap<String, String> {
    std::fs::read_to_string(dir.join("sim.env"))
        .unwrap_or_default()
        .lines()
        .filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_string(), v.to_string())))
        .collect()
}

fn uid() -> u32 {
    unsafe { libc::getuid() }
}

/// The ring path this instance should publish frames to (unique per phone)
pub fn ring_for(id: &str) -> PathBuf {
    let tag = id.strip_prefix("device-").unwrap_or(id);
    PathBuf::from(format!("/run/user/{}/x86code-{tag}-simfb.ring", uid()))
}

pub fn load(dir: &Path) -> Instance {
    let id = dir.file_name().unwrap_or_default().to_string_lossy().into_owned();
    let sfx = id.strip_prefix("device-a64").unwrap_or("");
    let is_default = sfx.is_empty();
    let env = env_of(dir);
    let name = std::fs::read_to_string(dir.join("name")).map(|s| s.trim().to_string()).unwrap_or_else(|_| if is_default { "Main phone".into() } else { id.clone() });
    let model_id = env.get("SIMULATOR_MODEL_IDENTIFIER").cloned().unwrap_or_default();
    let model = crate::sim::models().into_iter().find(|m| m.1 == model_id).map(|m| m.0).unwrap_or_else(|| if model_id.is_empty() { "iPhone".into() } else { model_id.clone() });
    let runtime_dir = std::fs::read_to_string(dir.join("runtime")).map(|s| s.trim().to_string()).ok().filter(|s| !s.is_empty()).unwrap_or_else(|| "ios-27.0-24A434-arm64".into());
    let runtime_root = repo_root().join("runtimes").join(&runtime_dir).join("RuntimeRoot");
    let runtime_present = crate::dyldcache::Cache::cache_dir(&runtime_root).join(crate::dyldcache::CACHE_NAME).exists();
    let (ios, build) = match runtime_dir.split('-').collect::<Vec<_>>()[..] {
        ["ios", v, b, ..] => (format!("iOS {v}"), b.to_string()),
        _ => (runtime_dir.clone(), String::new()),
    };
    let w = env.get("SIMULATOR_MAINSCREEN_WIDTH").and_then(|v| v.parse().ok()).unwrap_or(1206);
    let h = env.get("SIMULATOR_MAINSCREEN_HEIGHT").and_then(|v| v.parse().ok()).unwrap_or(2622);
    let scale = env.get("SIMULATOR_MAINSCREEN_SCALE").and_then(|v| v.parse().ok()).unwrap_or(3.0);
    let ring = env.get("SIMFB_RING").map(|r| PathBuf::from(r.strip_prefix("/Volumes/SystemRoot").unwrap_or(r))).unwrap_or_else(|| ring_for(&id));
    let prefix_sfx = if is_default { String::new() } else if sfx.starts_with('-') { sfx.to_string() } else { format!("-{sfx}") };
    Instance {
        sock: dir.join("data/simhid.sock"),
        log_dir: dir.join("data/var/log"),
        state_dir: repo_root().join("var").join(format!("run-a64{sfx}")),
        prefix: scratch().join(format!("a64prefix{prefix_sfx}")),
        udid: env.get("SIMULATOR_UDID").cloned().unwrap_or_default(),
        id,
        dir: dir.to_path_buf(),
        name,
        model,
        model_id,
        runtime_dir,
        runtime_root,
        runtime_present,
        ios,
        build,
        screen: (w, h),
        scale,
        ring,
        is_default,
    }
}

pub fn list() -> Vec<Instance> {
    let mut v: Vec<Instance> = crate::sim::devices().into_iter().map(|d| load(&d.1)).collect();
    v.sort_by(|a, b| b.is_default.cmp(&a.is_default).then(a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    v
}

/// Per-phone launcher settings (<device>/launcher.json): how it boots.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct BootSettings {
    /// X86CODE_METAL: all | backboardd | 0
    pub metal: String,
    /// X86SIM_MEM_MAX
    pub mem_max: String,
    /// A64_JOBS: std | full | slim | min
    pub jobs: String,
    /// A64_ENV extra K=V for launchd_sim
    pub extra_env: String,
    /// share one mtlvk between phones when one is up
    pub share_mtlvk: bool,
}

impl Default for BootSettings {
    fn default() -> Self {
        BootSettings { metal: "all".into(), mem_max: "12G".into(), jobs: "std".into(), extra_env: String::new(), share_mtlvk: true }
    }
}

pub fn settings_path(dir: &Path) -> PathBuf {
    dir.join("launcher.json")
}

pub fn load_settings(dir: &Path) -> BootSettings {
    std::fs::read(settings_path(dir)).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

pub fn save_settings(dir: &Path, s: &BootSettings) -> Result<()> {
    crate::aot::write_json(&settings_path(dir), s)
}

// ---- processes ----

#[derive(Debug, Clone, Default, Serialize)]
pub struct Proc {
    pub pid: u32,
    pub comm: String,
    pub cpu_pct: f64,
    pub pss_mb: f64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum Phase {
    Off,
    Starting,
    Running,
    Stopping,
}

#[derive(Debug, Clone, Serialize)]
pub struct Status {
    pub id: String,
    pub phase: Phase,
    pub home: bool,
    pub procs: Vec<Proc>,
    pub cpu_pct: f64,
    pub pss_mb: f64,
    /// seconds since launchd_sim started
    pub uptime_s: Option<f64>,
    /// the boot's launchd_sim start (unix seconds): frames older than it are another session's
    pub boot_time: Option<f64>,
    /// the dirs this boot really uses (a phone booted by hand with the default dirs)
    pub state_dir: PathBuf,
    pub prefix: PathBuf,
    pub mtlvk: bool,
    pub launchd_pid: Option<u32>,
}

fn read(p: &str) -> String {
    std::fs::read_to_string(p).unwrap_or_default()
}

struct ProcRow {
    comm: String,
    ppid: u32,
    cmd: String,
    ticks: u64,
    start_ticks: u64,
}

fn scan() -> HashMap<u32, ProcRow> {
    let mut m = HashMap::new();
    for e in std::fs::read_dir("/proc").into_iter().flatten().flatten() {
        let Ok(pid) = e.file_name().to_string_lossy().parse::<u32>() else { continue };
        let stat = read(&format!("/proc/{pid}/stat"));
        let Some(rest) = stat.rsplit(')').next() else { continue };
        let f: Vec<&str> = rest.split_whitespace().collect();
        let ppid = f.get(1).and_then(|x| x.parse().ok()).unwrap_or(0);
        let ticks = f.get(11).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0) + f.get(12).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0);
        let start_ticks = f.get(19).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0);
        let comm = stat.split_once('(').and_then(|(_, r)| r.rsplit_once(')')).map(|(c, _)| c.to_string()).unwrap_or_default();
        let cmd = if matches!(comm.as_str(), "launchd_sim" | "darlingserver" | "mtlvk" | "boot-a64.sh") { read(&format!("/proc/{pid}/cmdline")).replace('\0', " ") } else { String::new() };
        m.insert(pid, ProcRow { comm, ppid, cmd, ticks, start_ticks });
    }
    m
}

fn btime() -> f64 {
    read("/proc/stat").lines().find_map(|l| l.strip_prefix("btime ")).and_then(|v| v.trim().parse().ok()).unwrap_or(0.0)
}

fn pss_mb(pid: u32) -> f64 {
    for l in read(&format!("/proc/{pid}/smaps_rollup")).lines() {
        if let Some(v) = l.strip_prefix("Pss:") {
            return v.trim().trim_end_matches("kB").trim().parse::<f64>().unwrap_or(0.0) / 1024.0;
        }
    }
    0.0
}

/// Samples CPU between calls (keeps the last ticks per pid).
#[derive(Default)]
pub struct Monitor {
    last: HashMap<u32, (u64, std::time::Instant)>,
    home_seen: HashMap<String, u32>,
    pub stopping: HashMap<String, std::time::Instant>,
    pub starting: HashMap<String, std::time::Instant>,
}

impl Monitor {
    pub fn poll(&mut self, insts: &[Instance]) -> Vec<Status> {
        let procs = scan();
        let now = std::time::Instant::now();
        let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) }.max(1) as f64;
        let bt = btime();
        let mut children: HashMap<u32, Vec<u32>> = HashMap::new();
        for (pid, r) in &procs {
            children.entry(r.ppid).or_default().push(*pid);
        }
        let mut out = vec![];
        let mut seen_pids = std::collections::HashSet::new();
        for inst in insts {
            let marker = format!("/Volumes/SystemRoot{}/data/var/run/launchd_bootstrap.plist", inst.dir.display());
            let launchd = procs.iter().find(|(_, r)| r.comm == "launchd_sim" && r.cmd.contains(&marker)).map(|(p, _)| *p);
            let mut pids = vec![];
            let mut prefix = inst.prefix.clone();
            if let Some(l) = launchd {
                let mut work = vec![l];
                while let Some(p) = work.pop() {
                    pids.push(p);
                    work.extend(children.get(&p).into_iter().flatten().copied());
                }
                // ancestors up to the darlingserver of this prefix
                let mut p = procs.get(&l).map(|r| r.ppid).unwrap_or(0);
                while p > 1 {
                    let Some(r) = procs.get(&p) else { break };
                    pids.push(p);
                    if r.comm == "darlingserver" {
                        if let Some(pre) = r.cmd.split_whitespace().nth(1) {
                            prefix = PathBuf::from(pre);
                        }
                        break;
                    }
                    p = r.ppid;
                }
            }
            // which state dir this boot uses: the one whose `device` file names this phone
            let mut state_dir = inst.state_dir.clone();
            if read(&format!("{}/device", state_dir.display())).trim() != inst.dir.to_string_lossy() {
                let def = repo_root().join("var/run-a64");
                if read(&format!("{}/device", def.display())).trim() == inst.dir.to_string_lossy() {
                    state_dir = def;
                }
            }
            let mtlvk_pid: Option<u32> = read(&format!("{}/mtlvk.pid", state_dir.display())).trim().parse().ok();
            let mtlvk = mtlvk_pid.is_some_and(|p| procs.get(&p).is_some_and(|r| r.comm == "mtlvk"));
            if launchd.is_some() {
                if let Some(p) = mtlvk_pid.filter(|_| mtlvk) {
                    pids.push(p);
                }
            }
            let booting = procs.values().any(|r| r.comm == "boot-a64.sh" && !r.cmd.is_empty() && {
                // few bash processes: reading their environ is cheap
                let pid = procs.iter().find(|(_, x)| std::ptr::eq(*x, r)).map(|(p, _)| *p).unwrap_or(0);
                read(&format!("/proc/{pid}/environ")).split('\0').any(|kv| kv == format!("A64_DEVICE={}", inst.dir.display()))
            });
            let mut plist = vec![];
            let (mut cpu, mut pss) = (0.0, 0.0);
            for p in &pids {
                if !seen_pids.insert(*p) {
                    continue;
                }
                let Some(r) = procs.get(p) else { continue };
                let c = match self.last.get(p) {
                    Some((t0, at)) if r.ticks >= *t0 => (r.ticks - t0) as f64 / hz / now.duration_since(*at).as_secs_f64().max(0.05) * 100.0,
                    _ => 0.0,
                };
                self.last.insert(*p, (r.ticks, now));
                let m = pss_mb(*p);
                cpu += c;
                pss += m;
                plist.push(Proc { pid: *p, comm: r.comm.clone(), cpu_pct: c, pss_mb: m });
            }
            plist.sort_by(|a, b| b.pss_mb.partial_cmp(&a.pss_mb).unwrap_or(std::cmp::Ordering::Equal));
            let boot_time = launchd.and_then(|l| procs.get(&l)).map(|r| bt + r.start_ticks as f64 / hz);
            let uptime_s = boot_time.map(|b| (std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0) - b).max(0.0));
            let home = match launchd {
                Some(l) if self.home_seen.get(&inst.id) == Some(&l) => true,
                Some(l) => {
                    let ok = crate::ring::Ring::new(&inst.ring).home_frame(boot_time.unwrap_or(0.0) - 2.0);
                    if ok {
                        self.home_seen.insert(inst.id.clone(), l);
                    }
                    ok
                }
                None => false,
            };
            let stopping = self.stopping.get(&inst.id).is_some_and(|t| t.elapsed().as_secs() < 60);
            let starting_hint = self.starting.get(&inst.id).is_some_and(|t| t.elapsed().as_secs() < 240);
            let phase = if stopping && (launchd.is_some() || booting) {
                Phase::Stopping
            } else if launchd.is_some() {
                self.stopping.remove(&inst.id);
                self.starting.remove(&inst.id);
                if home { Phase::Running } else { Phase::Starting }
            } else if booting || starting_hint {
                Phase::Starting
            } else {
                self.stopping.remove(&inst.id);
                Phase::Off
            };
            out.push(Status { id: inst.id.clone(), phase, home, procs: plist, cpu_pct: cpu, pss_mb: pss, uptime_s, boot_time, state_dir, prefix, mtlvk, launchd_pid: launchd });
        }
        self.last.retain(|p, _| seen_pids.contains(p));
        out
    }
}

// ---- start / stop ----

fn boot_script() -> PathBuf {
    repo_root().join("cli/libexec/arm64/boot-a64.sh")
}

pub fn boot_log(inst: &Instance) -> PathBuf {
    scratch().join(format!("launcher-boot-{}.log", inst.id))
}

/// A live mtlvk socket to share: this phone's own (a restart keeps it), else any other running phone's.
fn shared_mtlvk(inst: &Instance, others: &[Status]) -> Option<PathBuf> {
    let live = |st: &Path| -> Option<PathBuf> {
        let pid: u32 = read(&format!("{}/mtlvk.pid", st.display())).trim().parse().ok()?;
        let comm = read(&format!("/proc/{pid}/comm"));
        let sock = st.join("mtlvk.sock");
        (comm.trim() == "mtlvk" && sock.exists()).then_some(sock)
    };
    if let Some(s) = live(&inst.state_dir) {
        return Some(s);
    }
    others.iter().filter(|s| s.id != inst.id && s.phase != Phase::Off).find_map(|s| live(&s.state_dir))
}

/// Boots a phone in its own prefix / state dir / ring. `others`: the current statuses (to share a running mtlvk).
pub fn start(inst: &Instance, others: &[Status]) -> Result<u32> {
    anyhow::ensure!(inst.runtime_present, "{} is not downloaded", inst.ios);
    let s = load_settings(&inst.dir);
    let log = std::fs::File::create(boot_log(inst))?;
    let mut cmd = Command::new("setsid");
    cmd.arg(boot_script())
        .current_dir(repo_root())
        .env("A64_KEEP", "1")
        .env("X86CODE_METAL", &s.metal)
        .env("X86SIM_MEM_MAX", &s.mem_max)
        .env("A64_JOBS", &s.jobs)
        .env("SECS", "20")
        .env("A64_DEVICE", &inst.dir)
        .env("A64_STATE", &inst.state_dir)
        .env("A64_DPREFIX", &inst.prefix)
        .env_remove("A64_RUNTIME")
        .env_remove("X86CODE_A64_JITCACHE_DIR")
        .stdin(Stdio::null())
        .stdout(log.try_clone()?)
        .stderr(log);
    if !s.extra_env.trim().is_empty() {
        cmd.env("A64_ENV", s.extra_env.trim());
    }
    if !inst.is_default {
        let ring = ring_for(&inst.id);
        cmd.env("SIMFB_RING", format!("/Volumes/SystemRoot{}", ring.display()));
        cmd.env("SIMFB_RING_LINUX", format!("/dev/shm/{}", ring.file_name().unwrap_or_default().to_string_lossy()));
    }
    if s.share_mtlvk && s.metal != "0" {
        if let Some(sock) = shared_mtlvk(inst, others) {
            cmd.env("A64_MTLVK_SOCKET", sock);
        }
    }
    Ok(cmd.spawn().context("starting boot-a64.sh")?.id())
}

/// Stops a phone's container only (the dirs the running boot really uses, from its status).
pub fn stop(inst: &Instance, st: Option<&Status>) -> Result<()> {
    let state_dir = st.map(|s| s.state_dir.clone()).unwrap_or_else(|| inst.state_dir.clone());
    let prefix = st.map(|s| s.prefix.clone()).unwrap_or_else(|| inst.prefix.clone());
    let out = Command::new(boot_script())
        .current_dir(repo_root())
        .env("A64_STOP_ONLY", "1")
        .env("A64_DEVICE", &inst.dir)
        .env("A64_STATE", &state_dir)
        .env("A64_DPREFIX", &prefix)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()?;
    anyhow::ensure!(out.status.success(), "stop failed: {}", String::from_utf8_lossy(&out.stderr).trim());
    Ok(())
}

/// Log sources of a phone: its job logs, the per-process translator logs of its prefix, the boot's state files.
pub fn log_sources(inst: &Instance, st: Option<&Status>) -> Vec<crate::logs::Src> {
    use crate::logs::Src;
    let (state_dir, prefix) = match st {
        Some(s) => (s.state_dir.clone(), s.prefix.clone()),
        None => (inst.state_dir.clone(), inst.prefix.clone()),
    };
    vec![
        Src::Dir("System", inst.log_dir.clone()),
        Src::Dir("Translator", prefix.join("private/var/tmp/x86code-a64")),
        Src::File("Boot", boot_log(inst)),
        Src::File("Boot", state_dir.join("lsim.out")),
        Src::File("Boot", state_dir.join("launchd.log")),
        Src::File("Boot", state_dir.join("aot-status.txt")),
        Src::File("Graphics", state_dir.join("mtlvk.log")),
        Src::File("Memory", state_dir.join("jetsam.log")),
    ]
}

// ---- rename / delete ----

/// Gives a phone a new display name (<device>/name).
pub fn rename(inst: &Instance, name: &str) -> Result<()> {
    let n = name.trim();
    anyhow::ensure!(!n.is_empty(), "the name is empty");
    anyhow::ensure!(n.chars().count() <= 60, "the name is too long");
    std::fs::write(inst.dir.join("name"), format!("{n}\n"))?;
    Ok(())
}

fn darlingserver_uses(prefix: &Path) -> bool {
    let want = prefix.to_string_lossy();
    std::fs::read_dir("/proc").into_iter().flatten().flatten().any(|e| {
        let p = e.path();
        read(&format!("{}/comm", p.display())).trim() == "darlingserver" && read(&format!("{}/cmdline", p.display())).split('\0').nth(1) == Some(want.as_ref())
    })
}

/// What `delete` removes (for the confirmation and the MCP/CLI): the device dir and, when they are this phone's own,
/// its Darling prefix, state dir, frame ring and boot log.
pub fn delete_plan(inst: &Instance) -> Vec<PathBuf> {
    let mut v = vec![inst.dir.clone()];
    let sfx = inst.id.strip_prefix("device-a64").unwrap_or("");
    if inst.is_default || sfx.is_empty() {
        return v;
    }
    let own_prefix = scratch().join(format!("a64prefix{}", if sfx.starts_with('-') { sfx.to_string() } else { format!("-{sfx}") }));
    if inst.prefix == own_prefix && inst.prefix.exists() {
        v.push(inst.prefix.clone());
    }
    let own_state = repo_root().join("var").join(format!("run-a64{sfx}"));
    if inst.state_dir == own_state && inst.state_dir.exists() {
        v.push(inst.state_dir.clone());
    }
    let ring = ring_for(&inst.id);
    if ring.exists() {
        v.push(ring.clone());
    }
    let link = PathBuf::from("/dev/shm").join(ring.file_name().unwrap_or_default());
    if link.symlink_metadata().is_ok() {
        v.push(link);
    }
    if boot_log(inst).exists() {
        v.push(boot_log(inst));
    }
    v
}

/// Removes a phone that is off. The default phone (its dirs are the scripts' defaults) is never removed, nor a dir a
/// running container uses.
pub fn delete(inst: &Instance, st: Option<&Status>) -> Result<()> {
    anyhow::ensure!(!inst.is_default, "the main phone can't be removed");
    anyhow::ensure!(st.is_none_or(|s| s.phase == Phase::Off && s.launchd_pid.is_none()), "stop {} first", inst.name);
    anyhow::ensure!(inst.dir.starts_with(repo_root().join("var")) && inst.id.starts_with("device-a64") && inst.id != "device-a64", "not a phone dir: {}", inst.dir.display());
    let plan = delete_plan(inst);
    for p in &plan {
        anyhow::ensure!(!(p.is_dir() && p.file_name().is_some_and(|n| n.to_string_lossy().starts_with("a64prefix")) && darlingserver_uses(p)), "{} is in use", p.display());
    }
    // the state dir's `device` file must name this phone (another boot may have borrowed it)
    for p in &plan {
        if p.file_name().is_some_and(|n| n.to_string_lossy().starts_with("run-a64")) {
            let dev = read(&format!("{}/device", p.display()));
            anyhow::ensure!(dev.trim().is_empty() || dev.trim() == inst.dir.to_string_lossy(), "{} belongs to {}", p.display(), dev.trim());
        }
    }
    for p in &plan {
        let r = if p.symlink_metadata().is_ok_and(|m| m.is_dir()) { std::fs::remove_dir_all(p) } else { std::fs::remove_file(p) };
        r.with_context(|| format!("removing {}", p.display()))?;
    }
    Ok(())
}
