//! The chunked AOT image.
//!
//! One chunk per library / executable: a dyld-cache dylib, a placed runtime Mach-O, or (app tier) a placed app
//! executable of the device. Each chunk is translated by libarm64jit's builder (X86CODE_A64_AOT_JOB, see
//! linux/arm64jit/jitcache.cpp) into its own *slot* of the code slab: translated code is slab-position dependent
//! (rel32 to the prelude, RIP-relative constants), so a slot is fixed once assigned and a rebuilt chunk goes back into
//! it. The image libarm64jit loads (`<uuid>-<build>.aot`, format A64JC06) is assembled here from the chunk files, so
//! the fast-load path (mapped `.map` / `.idx`, lazy registration) is unchanged.
//!
//! Layout under the jitcache dir J:
//!   J/placement.txt               non-cache Mach-O slots (cli/libexec/arm64/mkplacement.py)
//!   J/aot/config.json             runtime, training dirs, scope, budget
//!   J/aot/manifest.json           built chunks (key, slot, sizes) and the assembled image
//!   J/aot/chunks/<id>.<key>.aotc  chunk files (A64CK01)
//!   J/aot/progress.json           live build progress (GUI / CLI)
//!   J/<uuid>-<build>.aot          the image
use crate::discover::{self, Kind, Source};
use crate::dyldcache::Cache;
use crate::features;
use crate::hot::{self, Hot};
use crate::key;
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::io::{BufRead, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

/// libarm64jit's code cache is at most 1344 MB (rel32 reach of libarm64jit and glibc); 384 MB of it stays free for
/// each process's own translations.
pub const SLAB_MB: u64 = 1344;
pub const DEFAULT_BUDGET_MB: u64 = SLAB_MB - 384;
const HOT_BYTES_PER_BLOCK: f64 = 260.0;
/// x86 bytes per arm64 code byte for full static coverage (default until a measurement sets it in the manifest)
const DEFAULT_STATIC_RATIO: f64 = 3.0;
const SLOT_ALIGN: u64 = 64 << 10;
const POOL_TOTAL: u64 = 2 << 20;
const POOL_REGION: u64 = 32 << 10;

pub fn repo_root() -> PathBuf {
    if let Ok(r) = std::env::var("X86SIM_ROOT") {
        return PathBuf::from(r);
    }
    // tools/launcher/core -> repo
    let here = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    here.ancestors().nth(3).map(Path::to_path_buf).unwrap_or(here)
}

pub fn scratch() -> PathBuf {
    std::env::var("X86SIM_SCRATCH").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from("/home/bobby/scratch/x86code"))
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub runtime_root: PathBuf,
    /// device dir (app tier and feature profile); None: system tier only
    pub device: Option<PathBuf>,
    pub training: Vec<PathBuf>,
    /// "hot": training blocks only (the pre-Speed 18 image); "loaded": libraries any process loads (dependency
    /// closure of the placed executables plus every library with training blocks), hot first, then static code up to
    /// the budget; "all": every library
    pub scope: String,
    pub budget_mb: u64,
    /// leave out the chunks of features turned off in the device's features.json
    pub follow_features: bool,
    /// Darling prefix for builder processes (never the live sim's)
    pub test_prefix: PathBuf,
}

impl Default for Config {
    fn default() -> Self {
        let r = repo_root();
        let s = scratch();
        Config {
            runtime_root: r.join("runtimes/ios-27.0-24A434-arm64/RuntimeRoot"),
            device: Some(r.join("var/device-a64")),
            training: vec![s.join("s15/train5"), s.join("s17/train6")],
            scope: "hot".into(),
            budget_mb: DEFAULT_BUDGET_MB,
            follow_features: false,
            test_prefix: s.join("a64testprefix"),
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ChunkRec {
    pub name: String,
    pub kind: Option<Kind>,
    pub key: String,
    pub ident: String,
    pub addr: u64,
    pub codegen: u64,
    pub slot_off: u64,
    pub slot_size: u64,
    pub used: u64,
    pub blocks: u64,
    pub hot: u64,
    pub file: Option<String>,
    pub bytes: u64,
    /// "built" | "empty" (nothing persistable) | "truncated" (slot full: the rest stays lazy)
    pub state: String,
    pub secs: f64,
    pub pool: u64,
    pub built_at: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Image {
    pub file: String,
    pub size: u64,
    pub code_size: u64,
    pub chunks: usize,
    pub blocks: u64,
    pub built_at: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Manifest {
    pub version: u32,
    pub cache_uuid: String,
    pub static_ratio: Option<f64>,
    pub chunks: BTreeMap<String, ChunkRec>,
    pub image: Option<Image>,
}

pub struct Paths {
    pub jc: PathBuf,
    pub aot: PathBuf,
}

impl Paths {
    pub fn new(jc: &Path) -> Paths {
        Paths { jc: jc.to_path_buf(), aot: jc.join("aot") }
    }
    pub fn default_jc() -> PathBuf {
        std::env::var("X86CODE_A64_JITCACHE_DIR").map(PathBuf::from).unwrap_or_else(|_| scratch().join("jitcache"))
    }
    pub fn config(&self) -> PathBuf {
        self.aot.join("config.json")
    }
    pub fn manifest(&self) -> PathBuf {
        self.aot.join("manifest.json")
    }
    pub fn progress(&self) -> PathBuf {
        self.aot.join("progress.json")
    }
    pub fn chunks(&self) -> PathBuf {
        self.aot.join("chunks")
    }
    pub fn placement(&self) -> PathBuf {
        self.jc.join("placement.txt")
    }
    pub fn load_config(&self) -> Config {
        std::fs::read(self.config()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
    }
    pub fn save_config(&self, c: &Config) -> Result<()> {
        write_json(&self.config(), c)
    }
    pub fn load_manifest(&self) -> Manifest {
        std::fs::read(self.manifest()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
    }
}

pub fn write_json<T: Serialize>(p: &Path, v: &T) -> Result<()> {
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d)?;
    }
    let tmp = p.with_extension(format!("tmp.{}", std::process::id()));
    std::fs::write(&tmp, serde_json::to_vec_pretty(v)?)?;
    std::fs::rename(&tmp, p)?;
    Ok(())
}

fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// A chunk of the current plan.
#[derive(Debug, Clone)]
pub struct PlanChunk {
    pub src: Source,
    pub key: String,
    pub hot: Vec<(u64, u32)>,
    pub est_hot: u64,
    pub est_full: u64,
    /// slot size this plan wants for a fresh build
    pub want: u64,
    pub feature: Option<String>,
    pub placed_path: Option<PathBuf>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ChunkStatus {
    pub name: String,
    pub kind: Kind,
    /// "up-to-date" | "stale" | "missing"
    pub state: String,
    pub reasons: Vec<String>,
    pub hot_blocks: usize,
    pub blocks: u64,
    pub bytes: u64,
    pub slot_mb: f64,
    pub feature: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Status {
    pub cache_uuid: String,
    pub image: String,
    pub image_present: bool,
    /// the image on disk was assembled from exactly the up-to-date chunks
    pub image_current: bool,
    pub scope: String,
    pub budget_mb: u64,
    pub total: usize,
    pub up_to_date: usize,
    pub stale: usize,
    pub missing: usize,
    pub dropped: usize,
    pub training_files: usize,
    pub hot_blocks: usize,
    /// the installed translator's codegen version (and where it was read): keys and the image name follow it
    pub codegen: u64,
    pub codegen_source: key::CodegenSource,
    pub chunks: Vec<ChunkStatus>,
}

pub struct Plan {
    pub cfg: Config,
    pub cache_uuid: String,
    pub build: u64,
    pub image_name: String,
    pub chunks: Vec<PlanChunk>,
    pub hot: Hot,
}

/// Discover the chunk sources, the hot set and the scope (no translation).
pub fn plan(p: &Paths, cfg: &Config) -> Result<Plan> {
    let cache = Cache::open(&cfg.runtime_root).context("opening the dyld cache")?;
    let uuid = cache.uuid_hex();
    // the cache dir's own per-process JIT caches are training profiles too (the sim records them on every boot)
    let mut training = cfg.training.clone();
    if !training.contains(&p.jc) {
        training.push(p.jc.clone());
    }
    let hot = hot::load(&training, &uuid, &p.aot.join("hot.bin"))?;
    let mut sources = discover::cache_sources(&cache);
    let mut placed_paths = HashMap::new();
    let placements = discover::read_placement(&p.placement());
    let mut placed_deps: Vec<String> = Vec::new();
    for pl in &placements {
        let Some(path) = discover::placed_path(pl, &cfg.runtime_root, cfg.device.as_deref()) else { continue };
        if let Some(pf) = discover::placed_source(pl, &path)? {
            if let Ok(data) = std::fs::read(&path) {
                if let Some(img) = crate::macho::FileImage::parse(&data) {
                    placed_deps.extend(img.header.dylibs.iter().cloned());
                }
            }
            placed_paths.insert(pf.source.name.clone(), path);
            sources.push(pf.source);
        }
    }
    let ratio = p.load_manifest().static_ratio.unwrap_or(DEFAULT_STATIC_RATIO);
    // a measurement (x86code-aot measure: every chunk translated without a size limit) gives real sizes
    let measured: HashMap<String, (u64, u64)> = std::fs::read(p.aot.join("measure.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .and_then(|v| v["chunks"].as_array().cloned())
        .unwrap_or_default()
        .iter()
        .filter_map(|c| Some((c["name"].as_str()?.to_string(), (c["hot_used"].as_u64()?, c["used"].as_u64()?))))
        .collect();
    let feats = cfg.device.as_deref().map(features::load).unwrap_or_default();
    let mut chunks: Vec<PlanChunk> = sources
        .into_iter()
        .map(|src| {
            let h = hot.in_ranges(&src.ranges);
            let (est_hot, est_full) = match measured.get(&src.name) {
                Some(&(hu, fu)) => (hu, fu.max(hu)),
                None => {
                    let eh = (h.len() as f64 * HOT_BYTES_PER_BLOCK) as u64;
                    (eh, ((src.code_bytes as f64 * ratio) as u64).max(eh))
                }
            };
            PlanChunk {
                key: key::chunk_key(&src.name, &src.ident, src.addr),
                feature: features::feature_of_chunk(&src.name),
                placed_path: placed_paths.get(&src.name).cloned(),
                src,
                hot: h,
                est_hot,
                est_full,
                want: 0,
            }
        })
        .collect();
    // scope
    let loaded: HashSet<String> = if cfg.scope == "loaded" { dependency_closure(&cache, &placed_deps) } else { HashSet::new() };
    chunks.retain(|c| match cfg.scope.as_str() {
        "hot" => !c.hot.is_empty(),
        "all" => true,
        _ => !c.hot.is_empty() || c.src.kind != Kind::Cache || loaded.contains(&c.src.name),
    });
    if cfg.follow_features {
        chunks.retain(|c| c.feature.as_ref().is_none_or(|f| feats.enabled(f)));
    }
    allocate(&mut chunks, cfg);
    let build = key::build_id(&p.placement());
    Ok(Plan { image_name: key::image_name(&uuid, build), cache_uuid: uuid, build, chunks, hot, cfg: cfg.clone() })
}

fn dependency_closure(cache: &Cache, roots: &[String]) -> HashSet<String> {
    let by_path: HashMap<&str, &crate::dyldcache::CacheImage> = cache.images.iter().map(|i| (i.path.as_str(), i)).collect();
    let mut seen: HashSet<String> = HashSet::new();
    let mut work: Vec<String> = roots.to_vec();
    while let Some(n) = work.pop() {
        if !seen.insert(n.clone()) {
            continue;
        }
        if let Some(h) = by_path.get(n.as_str()).and_then(|i| cache.header(i)) {
            work.extend(h.dylibs.iter().filter(|d| !seen.contains(*d)).cloned());
        }
    }
    seen
}

/// Slot sizes for fresh builds: every chunk gets its hot part; the static rest is granted by hot density (hot blocks
/// per code byte: the most used libraries get full coverage first) until the budget is used up. Order = priority.
fn allocate(chunks: &mut [PlanChunk], cfg: &Config) {
    let budget = cfg.budget_mb << 20;
    let align = |v: u64| v.div_ceil(SLOT_ALIGN) * SLOT_ALIGN;
    let mut used = 0u64;
    for c in chunks.iter_mut() {
        c.want = if c.hot.is_empty() { 0 } else { align(c.est_hot + c.est_hot / 4 + (64 << 10)) };
        used += c.want;
    }
    if cfg.scope != "hot" {
        let mut order: Vec<usize> = (0..chunks.len()).collect();
        let density = |c: &PlanChunk| c.hot.len() as f64 / c.src.code_bytes.max(1) as f64;
        order.sort_by(|&a, &b| density(&chunks[b]).total_cmp(&density(&chunks[a])).then(chunks[a].src.name.cmp(&chunks[b].src.name)));
        for i in order {
            let c = &mut chunks[i];
            let extra = align((c.est_full.saturating_sub(c.est_hot)) * 9 / 8 + if c.want == 0 { 64 << 10 } else { 0 });
            let grant = extra.min(budget.saturating_sub(used));
            if grant >= SLOT_ALIGN * 2 || (grant == extra && grant > 0) {
                c.want += grant;
                used += grant;
            }
        }
    }
    // hot libraries first (contiguous hot code), then by name
    chunks.sort_by(|a, b| b.hot.len().cmp(&a.hot.len()).then(a.src.name.cmp(&b.src.name)));
}

impl Plan {
    pub fn status(&self, p: &Paths) -> Status {
        let m = p.load_manifest();
        let mut st = Status {
            cache_uuid: self.cache_uuid.clone(),
            image: self.image_name.clone(),
            image_present: p.jc.join(&self.image_name).exists(),
            image_current: false,
            scope: self.cfg.scope.clone(),
            budget_mb: self.cfg.budget_mb,
            total: 0,
            up_to_date: 0,
            stale: 0,
            missing: 0,
            dropped: 0,
            training_files: self.hot.files,
            hot_blocks: self.hot.blocks.len(),
            codegen: key::codegen_info().0,
            codegen_source: key::codegen_info().1,
            chunks: vec![],
        };
        for c in &self.chunks {
            if c.want == 0 {
                st.dropped += 1;
                continue;
            }
            let (state, reasons) = chunk_state(p, &m, c);
            match state.as_str() {
                "up-to-date" => st.up_to_date += 1,
                "stale" => st.stale += 1,
                _ => st.missing += 1,
            }
            let rec = m.chunks.get(&c.src.name);
            st.chunks.push(ChunkStatus {
                name: c.src.name.clone(),
                kind: c.src.kind.clone(),
                state,
                reasons,
                hot_blocks: c.hot.len(),
                blocks: rec.map(|r| r.blocks).unwrap_or(0),
                bytes: rec.map(|r| r.bytes).unwrap_or(0),
                slot_mb: rec.map(|r| r.slot_size).unwrap_or(c.want) as f64 / 1048576.0,
                feature: c.feature.clone(),
            });
        }
        st.total = st.chunks.len();
        st.image_current = st.image_present
            && st.stale == 0
            && st.missing == 0
            && m.image.as_ref().is_some_and(|i| i.file == self.image_name && i.chunks == self.built_records(&m).len());
        st
    }

    fn built_records<'a>(&self, m: &'a Manifest) -> Vec<&'a ChunkRec> {
        self.chunks
            .iter()
            .filter(|c| c.want > 0)
            .filter_map(|c| m.chunks.get(&c.src.name).filter(|r| r.key == c.key && r.file.is_some()))
            .collect()
    }
}

fn chunk_state(p: &Paths, m: &Manifest, c: &PlanChunk) -> (String, Vec<String>) {
    let Some(r) = m.chunks.get(&c.src.name) else { return ("missing".into(), vec![]) };
    if r.key == c.key {
        let present = r.file.as_ref().is_none_or(|f| p.chunks().join(f).exists());
        return if present { ("up-to-date".into(), vec![]) } else { ("missing".into(), vec!["chunk file deleted".into()]) };
    }
    let mut why = vec![];
    if r.ident != c.src.ident {
        why.push(match c.src.kind {
            Kind::Cache => "runtime update (library changed)".to_string(),
            Kind::Placed => "runtime update (file changed)".to_string(),
            Kind::App => "app changed".to_string(),
        });
    }
    if r.addr != c.src.addr {
        why.push("load address moved (placement / cache layout)".into());
    }
    if r.codegen != key::codegen() {
        why.push(format!("translator update (codegen {:#x} -> {:#x})", r.codegen, key::codegen()));
    }
    if why.is_empty() {
        why.push("key changed".into());
    }
    ("stale".into(), why)
}

// ---------------------------------------------------------------------------------------------------------------
// Building

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Progress {
    /// "idle" | "planning" | "building" | "assembling" | "done" | "cancelled" | "error"
    pub state: String,
    pub message: String,
    pub current: Vec<String>,
    pub chunks_done: usize,
    pub chunks_total: usize,
    pub chunks_skipped: usize,
    pub blocks: u64,
    pub mb_written: f64,
    pub elapsed_s: f64,
    pub eta_s: Option<f64>,
    pub workers: usize,
    pub pid: u32,
    pub updated: u64,
}

pub struct BuildOpts {
    pub workers: usize,
    /// relayout every chunk (fresh slots, fresh constant pool)
    pub full: bool,
    /// measurement: every chunk at slot 0 with the whole slab, written to aot/measure (no image)
    pub measure: bool,
    pub cancel: Arc<AtomicBool>,
    pub only: Option<Vec<String>>,
}

impl Default for BuildOpts {
    fn default() -> Self {
        let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
        BuildOpts { workers: (n / 2).max(1), full: false, measure: false, cancel: Arc::new(AtomicBool::new(false)), only: None }
    }
}

struct Job {
    chunks: Vec<(String, u64, u64)>, // name, slot_off, slot_size
}

fn write_entries(path: &Path, c: &PlanChunk, statics: &[u64]) -> Result<()> {
    let mut b: Vec<u8> = Vec::with_capacity(8 * (4 + c.src.ranges.len() * 2 + c.hot.len() + statics.len()));
    let mut put = |v: u64| b.extend_from_slice(&v.to_le_bytes());
    put(c.src.ranges.len() as u64);
    for &(lo, hi) in &c.src.ranges {
        put(lo);
        put(hi);
    }
    put(c.hot.len() as u64);
    for &(d, _) in &c.hot {
        put(d);
    }
    put(statics.len() as u64);
    for &s in statics {
        put(s);
    }
    std::fs::write(path, b)?;
    Ok(())
}

/// Build what is stale or missing (all with opts.full), then assemble the image. Progress goes to aot/progress.json
/// (and to `on_progress`).
pub fn build(p: &Paths, cfg: &Config, opts: &BuildOpts, on_progress: &mut dyn FnMut(&Progress)) -> Result<Progress> {
    let t0 = Instant::now();
    let mut prog = Progress { state: "planning".into(), pid: std::process::id(), workers: opts.workers, ..Default::default() };
    let publish = |prog: &mut Progress, on: &mut dyn FnMut(&Progress)| {
        prog.elapsed_s = t0.elapsed().as_secs_f64();
        prog.updated = now();
        let _ = write_json(&p.progress(), prog);
        on(prog);
    };
    publish(&mut prog, on_progress);
    let plan = plan(p, cfg)?;
    let cache = Cache::open(&cfg.runtime_root)?;
    let mut man = p.load_manifest();
    if man.cache_uuid != plan.cache_uuid || opts.full {
        // a new runtime keeps chunks whose library is unchanged (same key); slots stay as they are
        man.cache_uuid = plan.cache_uuid.clone();
    }
    if opts.full && !opts.measure {
        man.chunks.clear();
    }
    let todo: Vec<&PlanChunk> = plan
        .chunks
        .iter()
        .filter(|c| c.want > 0 || opts.measure)
        .filter(|c| opts.only.as_ref().is_none_or(|o| o.iter().any(|n| c.src.name.contains(n.as_str()))))
        .filter(|c| opts.measure || chunk_state(p, &man, c).0 != "up-to-date")
        .collect();
    prog.chunks_total = todo.len();
    if todo.is_empty() && !opts.measure && plan.status(p).image_current {
        prog.state = "done".into();
        prog.chunks_skipped = plan.chunks.iter().filter(|c| c.want > 0).count();
        prog.message = format!("image {} is up to date ({} chunks unchanged)", plan.image_name, prog.chunks_skipped);
        publish(&mut prog, on_progress);
        return Ok(prog);
    }
    prog.chunks_skipped = plan.chunks.iter().filter(|c| c.want > 0).count().saturating_sub(todo.len());
    prog.state = "building".into();
    prog.message = format!("{} of {} chunks need rebuilding", todo.len(), todo.len() + prog.chunks_skipped);
    publish(&mut prog, on_progress);

    let slab = SLAB_MB << 20;
    let out_dir = if opts.measure { p.aot.join("measure") } else { p.chunks() };
    std::fs::create_dir_all(&out_dir)?;
    let work = p.aot.join("work");
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work)?;

    // slots: keep assigned ones (incremental), give new chunks room after the last slot
    let mut end = man.chunks.values().map(|r| r.slot_off + r.slot_size).max().unwrap_or(0);
    let mut slots: HashMap<String, (u64, u64)> = HashMap::new();
    for c in &todo {
        if opts.measure {
            slots.insert(c.src.name.clone(), (0, slab - (8 << 20)));
            continue;
        }
        if let Some(r) = man.chunks.get(&c.src.name).filter(|r| r.slot_size > 0 && r.state != "truncated" || r.slot_size >= c.want) {
            slots.insert(c.src.name.clone(), (r.slot_off, r.slot_size));
        } else {
            let size = c.want;
            if end + size > slab - (8 << 20) {
                bail!("the slab is full ({} MB used): run a full rebuild (x86code-aot build --full) to compact it", end >> 20);
            }
            slots.insert(c.src.name.clone(), (end, size));
            end += size;
        }
    }

    // balance the chunks over the workers by slot size
    let w = opts.workers.clamp(1, 16).min(todo.len().max(1));
    let mut jobs: Vec<Job> = (0..w).map(|_| Job { chunks: vec![] }).collect();
    let mut load = vec![0u64; w];
    let mut order: Vec<&&PlanChunk> = todo.iter().collect();
    order.sort_by_key(|c| std::cmp::Reverse(slots[&c.src.name].1.min(c.est_full.max(c.est_hot))));
    for c in order {
        let i = (0..w).min_by_key(|&i| load[i]).unwrap();
        let (off, size) = slots[&c.src.name];
        load[i] += c.est_full.min(size);
        jobs[i].chunks.push((c.src.name.clone(), off, size));
    }

    // entries files (static discovery)
    let by_name: HashMap<&str, &PlanChunk> = plan.chunks.iter().map(|c| (c.src.name.as_str(), c)).collect();
    let placements: HashMap<String, discover::Placement> = discover::read_placement(&p.placement()).into_iter().map(|x| (x.key.clone(), x)).collect();
    let mut ent_paths: HashMap<String, PathBuf> = HashMap::new();
    for c in &todo {
        let statics = if cfg.scope == "hot" && !opts.measure {
            vec![]
        } else {
            match c.src.kind {
                Kind::Cache => discover::cache_entries(&cache, &c.src),
                _ => match (placements.get(&c.src.name), &c.placed_path) {
                    (Some(pl), Some(path)) => discover::placed_entries(pl, path),
                    _ => vec![],
                },
            }
        };
        let ep = work.join(format!("{}.ent", key::name_id(&c.src.name)));
        write_entries(&ep, c, &statics)?;
        ent_paths.insert(c.src.name.clone(), ep);
    }

    // constant pool: the union of every kept chunk's pool; new constants from the largest pool end on, a disjoint
    // region per worker
    let kept: Vec<PathBuf> = if opts.full || opts.measure {
        vec![]
    } else {
        man.chunks.values().filter(|r| !todo.iter().any(|c| c.src.name == r.name)).filter_map(|r| r.file.as_ref().map(|f| p.chunks().join(f))).collect()
    };
    let pool_base = if opts.full || opts.measure { 0 } else { man.chunks.values().map(|r| r.pool).max().unwrap_or(0) }.div_ceil(16) * 16;
    if pool_base + POOL_REGION * w as u64 > POOL_TOTAL - (64 << 10) {
        bail!("the constant pool is nearly full: run a full rebuild (x86code-aot build --full)");
    }

    let repo = repo_root();
    let mut children: Vec<(Child, PathBuf, usize)> = vec![];
    for (i, j) in jobs.iter().enumerate() {
        if j.chunks.is_empty() {
            continue;
        }
        let jp = work.join(format!("w{i}.job"));
        let pp = work.join(format!("w{i}.progress"));
        let mut s = format!("progress {}\n", pp.display());
        for k in &kept {
            s += &format!("pool {}\n", k.display());
        }
        s += &format!("region {}\n", pool_base + POOL_REGION * i as u64);
        for (name, off, size) in &j.chunks {
            let c = by_name[name.as_str()];
            let out = out_dir.join(format!("{}.{}.aotc", key::name_id(name), c.key));
            s += &format!("chunk {} {} {} {} {}\n", name.replace(' ', "%20"), off, size, out.display(), ent_paths[name].display());
        }
        std::fs::write(&jp, s)?;
        let log = std::fs::File::create(work.join(format!("w{i}.log")))?;
        // capped and low priority: nice 19, idle IO class; one builder per core
        let mut cmd = Command::new("nice");
        cmd.args(["-n", "19", "ionice", "-c", "3"])
            .arg(repo.join("tests/arm64/run.sh"))
            .arg(repo.join("tests/arm64/icstress.sim"))
            .current_dir(&repo)
            .env("A64_DPREFIX", &cfg.test_prefix)
            // Speed 23: the builder maps this runtime's cache (run.sh defaults to 27.0: another runtime's image
            // was 27.0 code under the other cache's UUID, and its processes jumped to garbage)
            .env("A64_RUNTIME", &cfg.runtime_root)
            .env("TIMEOUT", "86400")
            .env("X86CODE_A64_AOT_BUILD", "1")
            .env("X86CODE_A64_AOT_JOB", &jp)
            .env("X86CODE_A64_JITCACHE_DIR", &p.jc)
            .env("X86CODE_A64_CODECACHE_MB", SLAB_MB.to_string())
            .stdin(Stdio::null())
            .stdout(log.try_clone()?)
            .stderr(log);
        unsafe {
            use std::os::unix::process::CommandExt;
            cmd.pre_exec(|| {
                libc::setpgid(0, 0);
                Ok(())
            });
        }
        children.push((cmd.spawn().context("starting a builder")?, pp, i));
        std::thread::sleep(std::time::Duration::from_millis(1500)); // the first one starts the container
    }

    // follow the workers' progress files
    let mut offsets: HashMap<usize, u64> = HashMap::new();
    let mut current: HashMap<usize, String> = HashMap::new();
    let mut partial: HashMap<usize, (u64, u64)> = HashMap::new(); // blocks, used of the running chunk
    let mut done_blocks = 0u64;
    let mut done_bytes = 0u64;
    let mut done_est = 0u64;
    let total_est: u64 = todo.iter().map(|c| c.est_full.min(slots[&c.src.name].1)).sum();
    let mut measured: Vec<serde_json::Value> = vec![];
    let mut codegen_checked = false;
    let mut cancelled = false;
    loop {
        let mut alive = 0;
        for (ch, pp, i) in children.iter_mut() {
            if ch.try_wait()?.is_none() {
                alive += 1;
            }
            let Ok(f) = std::fs::File::open(&*pp) else { continue };
            let off = offsets.entry(*i).or_default();
            let mut r = std::io::BufReader::new(f);
            use std::io::Seek;
            r.seek(std::io::SeekFrom::Start(*off))?;
            let mut line = String::new();
            while r.read_line(&mut line)? > 0 {
                if !line.ends_with('\n') {
                    break; // partial line: read it next time
                }
                *off += line.len() as u64;
                let Ok(v) = serde_json::from_str::<serde_json::Value>(line.trim()) else {
                    line.clear();
                    continue;
                };
                let ev = v["event"].as_str().unwrap_or("");
                let name = v["name"].as_str().unwrap_or("").replace("%20", " ");
                match ev {
                    "start" => {
                        if let Some(cg) = v["codegen"].as_u64() {
                            if cg != key::codegen() && !codegen_checked {
                                for (ch, _, _) in children.iter_mut() {
                                    unsafe {
                                        libc::kill(-(ch.id() as i32), libc::SIGTERM);
                                    }
                                    let _ = ch.wait();
                                }
                                bail!("the translator reports codegen {cg:#x}, but {} reads {:#x}: was it rebuilt during the run? Start again", key::translator().display(), key::codegen());
                            }
                            codegen_checked = true;
                        }
                    }
                    "progress" => {
                        current.insert(*i, name.clone());
                        partial.insert(*i, (v["blocks"].as_u64().unwrap_or(0), v["used"].as_u64().unwrap_or(0)));
                    }
                    "chunk" => {
                        partial.remove(i);
                        current.remove(i);
                        let c = by_name[name.as_str()];
                        let blocks = v["blocks"].as_u64().unwrap_or(0);
                        let bytes = v["bytes"].as_u64().unwrap_or(0);
                        let used = v["used"].as_u64().unwrap_or(0);
                        let state = v["state"].as_str().unwrap_or("error");
                        done_blocks += blocks;
                        done_bytes += bytes;
                        done_est += c.est_full.min(slots[&name].1);
                        prog.chunks_done += 1;
                        if opts.measure {
                            measured.push(serde_json::json!({"name": name, "blocks": blocks, "bytes": bytes, "used": used,
                                "hot_used": v["hot_used"], "hot_blocks": v["hot_blocks"],
                                "code": c.src.code_bytes, "hot": c.hot.len(), "secs": v["secs"], "kind": c.src.kind}));
                            let _ = std::fs::remove_file(out_dir.join(format!("{}.{}.aotc", key::name_id(&name), c.key)));
                        } else if state == "done" || state == "empty" {
                            let (off, size) = slots[&name];
                            let file = (state == "done").then(|| format!("{}.{}.aotc", key::name_id(&name), c.key));
                            // drop the replaced chunk file
                            if let Some(old) = man.chunks.get(&name).and_then(|r| r.file.clone()) {
                                if Some(&old) != file.as_ref() {
                                    let _ = std::fs::remove_file(p.chunks().join(old));
                                }
                            }
                            man.chunks.insert(
                                name.clone(),
                                ChunkRec {
                                    name: name.clone(),
                                    kind: Some(c.src.kind.clone()),
                                    key: c.key.clone(),
                                    ident: c.src.ident.clone(),
                                    addr: c.src.addr,
                                    codegen: key::codegen(),
                                    slot_off: off,
                                    slot_size: size,
                                    used,
                                    blocks,
                                    hot: c.hot.len() as u64,
                                    file,
                                    bytes,
                                    state: if v["overflow"].as_bool() == Some(true) { "truncated".into() } else if state == "done" { "built".into() } else { "empty".into() },
                                    secs: v["secs"].as_f64().unwrap_or(0.0),
                                    pool: v["pool"].as_u64().unwrap_or(0),
                                    built_at: now(),
                                },
                            );
                            write_json(&p.manifest(), &man)?; // resumable: every finished chunk is recorded
                        } else {
                            prog.message = format!("chunk {name} failed: {}", v["why"].as_str().unwrap_or(state));
                        }
                    }
                    _ => {}
                }
                line.clear();
            }
        }
        let pb: u64 = partial.values().map(|x| x.0).sum();
        let pu: u64 = partial.values().map(|x| x.1).sum();
        prog.blocks = done_blocks + pb;
        prog.mb_written = done_bytes as f64 / 1048576.0;
        let mut cur: Vec<String> = current.values().cloned().collect();
        cur.sort();
        prog.current = cur;
        let frac = (done_est + pu) as f64 / total_est.max(1) as f64;
        let el = t0.elapsed().as_secs_f64();
        prog.eta_s = (frac > 0.02).then(|| el / frac * (1.0 - frac).max(0.0));
        publish(&mut prog, on_progress);
        if opts.cancel.load(Ordering::Relaxed) {
            cancelled = true;
            break;
        }
        if alive == 0 {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    if cancelled {
        for (ch, _, _) in children.iter_mut() {
            let pid = ch.id() as i32;
            unsafe {
                libc::kill(-pid, libc::SIGTERM);
            }
            let _ = ch.wait();
        }
    }
    shutdown_test_prefix(cfg);
    if cancelled {
        prog.state = "cancelled".into();
        prog.message = format!("cancelled after {} of {} chunks (resume picks up the rest)", prog.chunks_done, prog.chunks_total);
        publish(&mut prog, on_progress);
        return Ok(prog);
    }
    if opts.measure {
        let tot_bytes: u64 = measured.iter().map(|m| m["bytes"].as_u64().unwrap_or(0)).sum();
        let tot_code: u64 = measured.iter().map(|m| m["code"].as_u64().unwrap_or(0)).sum();
        if tot_code > 0 {
            man.static_ratio = Some(tot_bytes as f64 / tot_code as f64);
            write_json(&p.manifest(), &man)?;
        }
        write_json(&p.aot.join("measure.json"), &serde_json::json!({"scope": cfg.scope, "workers": w, "secs": t0.elapsed().as_secs_f64(),
            "bytes": tot_bytes, "blocks": done_blocks, "code": tot_code, "chunks": measured}))?;
        prog.state = "done".into();
        prog.message = format!("measured {} chunks: {:.0} MB, {} blocks", measured.len(), tot_bytes as f64 / 1048576.0, done_blocks);
        publish(&mut prog, on_progress);
        return Ok(prog);
    }
    if prog.chunks_done < prog.chunks_total {
        prog.state = "error".into();
        if prog.message.is_empty() || !prog.message.starts_with("chunk") {
            prog.message = format!("only {} of {} chunks finished: see {}", prog.chunks_done, prog.chunks_total, work.display());
        }
        publish(&mut prog, on_progress);
        return Ok(prog);
    }
    prog.state = "assembling".into();
    publish(&mut prog, on_progress);
    let img = assemble(p, &plan, &mut man)?;
    write_json(&p.manifest(), &man)?;
    let _ = std::fs::remove_dir_all(&work);
    prog.state = "done".into();
    prog.message = format!("image {} ({:.0} MB code, {} chunks, {} blocks)", img.file, img.code_size as f64 / 1048576.0, img.chunks, img.blocks);
    publish(&mut prog, on_progress);
    Ok(prog)
}

pub fn shutdown_test_prefix(cfg: &Config) {
    let darling = repo_root().join("darling-prefix/bin/darling");
    let _ = Command::new(darling).arg("shutdown").env("DPREFIX", &cfg.test_prefix).stdout(Stdio::null()).stderr(Stdio::null()).status();
}

// ---------------------------------------------------------------------------------------------------------------
// Assembly (A64CK01 chunks -> one A64JC06 image). Layout: PersistHeader (160 bytes: magic, uuid, build_id, code_size,
// n_blocks, prelude_size, n_relocs, n_fastmem, old_bases[9], const_pool_used, n_links), [chunk: u64 code_off],
// blocks (24 B), relocs (8 B), fastmem sites (24 B), links (16 B), code, constant pool.

const HDR: usize = 160;
const NB: usize = 9;

struct ChunkFile {
    data: memmap2::Mmap,
    code_off: u64,
    code_size: u64,
    n: [u64; 4], // blocks, relocs, fastmem, links
    prelude: u64,
    bases: [u64; NB],
    pool_used: u64,
    build: u64,
}

impl ChunkFile {
    fn open(path: &Path) -> Result<ChunkFile> {
        let f = std::fs::File::open(path).with_context(|| format!("opening {}", path.display()))?;
        let data = unsafe { memmap2::Mmap::map(&f)? };
        if data.len() < HDR + 8 || &data[..8] != b"A64CK01\0" {
            bail!("{} is not a chunk", path.display());
        }
        let g = |o: usize| u64::from_le_bytes(data[o..o + 8].try_into().unwrap());
        let mut bases = [0u64; NB];
        for (k, b) in bases.iter_mut().enumerate() {
            *b = g(72 + 8 * k);
        }
        let cf = ChunkFile {
            code_off: g(HDR),
            code_size: g(32),
            n: [g(40), g(56), g(64), g(152)],
            prelude: g(48),
            bases,
            pool_used: g(144),
            build: g(24),
            data,
        };
        if cf.code_off > cf.code_size || cf.pool_end() > cf.data.len() {
            bail!("{} is truncated", path.display());
        }
        Ok(cf)
    }
    fn sect(&self, i: usize) -> (usize, usize) {
        let sz = [24usize, 8, 24, 16];
        let mut o = HDR + 8;
        for k in 0..i {
            o += self.n[k] as usize * sz[k];
        }
        (o, self.n[i] as usize * sz[i])
    }
    fn code(&self) -> (usize, usize) {
        let (o, l) = self.sect(3);
        (o + l, (self.code_size - self.code_off) as usize)
    }
    fn pool_end(&self) -> usize {
        let (o, l) = self.code();
        o + l + self.pool_used as usize
    }
}

pub fn assemble(p: &Paths, plan: &Plan, man: &mut Manifest) -> Result<Image> {
    let mut recs: Vec<ChunkRec> = plan.built_records(man).into_iter().cloned().collect();
    recs.sort_by_key(|r| r.slot_off);
    let files: Vec<ChunkFile> = recs.iter().map(|r| ChunkFile::open(&p.chunks().join(r.file.as_ref().unwrap()))).collect::<Result<_>>()?;
    if files.is_empty() {
        bail!("no chunks to assemble");
    }
    let f0 = &files[0];
    let cb = 0usize;
    let mut code_size = 0u64;
    let mut n = [0u64; 4];
    let mut pool: Vec<u8> = vec![];
    let mut last_end = 0u64;
    for (f, r) in files.iter().zip(&recs) {
        if f.prelude != f0.prelude {
            bail!("{}: prelude size differs", r.name);
        }
        for k in 1..=4 {
            if f.bases[k].wrapping_sub(f.bases[cb]) != f0.bases[k].wrapping_sub(f0.bases[cb]) {
                bail!("{}: prelude layout differs", r.name);
            }
        }
        for k in 0..NB {
            if (f.bases[k] == 0) != (f0.bases[k] == 0) {
                bail!("{}: reloc kinds differ", r.name);
            }
        }
        if f.build & 0x3_ffff != f0.build & 0x3_ffff {
            bail!("{}: codegen / flags differ", r.name);
        }
        if f.code_off < last_end {
            bail!("{}: slot overlaps the previous chunk", r.name);
        }
        last_end = f.code_size;
        code_size = code_size.max(f.code_size);
        for k in 0..4 {
            n[k] += f.n[k];
        }
        let (co, cl) = f.code();
        let fp = &f.data[co + cl..co + cl + f.pool_used as usize];
        if fp.len() > pool.len() {
            pool.resize(fp.len(), 0);
        }
        for i in (0..fp.len() / 16 * 16).step_by(16) {
            let s = &fp[i..i + 16];
            if s.iter().all(|&b| b == 0) {
                continue;
            }
            let d = &mut pool[i..i + 16];
            if d.iter().any(|&b| b != 0) && d != s {
                bail!("{}: constant pool conflict at {i:#x}", r.name);
            }
            d.copy_from_slice(s);
        }
    }
    let out = p.jc.join(&plan.image_name);
    let tmp = out.with_extension(format!("aot.tmp.{}", std::process::id()));
    let mut w = std::io::BufWriter::with_capacity(8 << 20, std::fs::File::create(&tmp)?);
    let mut hdr = f0.data[..HDR].to_vec();
    hdr[..8].copy_from_slice(b"A64JC06\0");
    let uuid = (0..16).map(|i| u8::from_str_radix(&plan.cache_uuid[2 * i..2 * i + 2], 16).unwrap_or(0)).collect::<Vec<_>>();
    hdr[8..24].copy_from_slice(&uuid);
    hdr[24..32].copy_from_slice(&plan.build.to_le_bytes());
    let put = |h: &mut Vec<u8>, o: usize, v: u64| h[o..o + 8].copy_from_slice(&v.to_le_bytes());
    put(&mut hdr, 32, code_size);
    put(&mut hdr, 40, n[0]);
    put(&mut hdr, 56, n[1]);
    put(&mut hdr, 64, n[2]);
    put(&mut hdr, 152, n[3]);
    put(&mut hdr, 144, pool.len() as u64);
    w.write_all(&hdr)?;
    for s in 0..4 {
        for f in &files {
            let (o, l) = f.sect(s);
            w.write_all(&f.data[o..o + l])?;
        }
    }
    // code: each chunk's bytes at its slot, absolute embeds rebased to the first chunk's bases, holes zero
    let mut at = 0u64;
    for f in &files {
        // slot headroom: a hole (sparse file; reads back as zeros) instead of written zeros
        if at < f.code_off {
            use std::io::Seek;
            w.seek(std::io::SeekFrom::Current((f.code_off - at) as i64))?;
            at = f.code_off;
        }
        let (co, cl) = f.code();
        let mut code = f.data[co..co + cl].to_vec();
        let (ro, rl) = f.sect(1);
        for e in f.data[ro..ro + rl].chunks_exact(8) {
            let off = u32::from_le_bytes(e[..4].try_into().unwrap()) as u64;
            let kind = e[4] as usize;
            if kind >= NB || off < f.code_off || off + 8 > f.code_size {
                continue;
            }
            let i = (off - f.code_off) as usize;
            let v = u64::from_le_bytes(code[i..i + 8].try_into().unwrap()).wrapping_add(f0.bases[kind].wrapping_sub(f.bases[kind]));
            code[i..i + 8].copy_from_slice(&v.to_le_bytes());
        }
        w.write_all(&code)?;
        at += cl as u64;
    }
    w.write_all(&pool)?;
    w.flush()?;
    drop(w);
    std::fs::rename(&tmp, &out)?;
    let size = std::fs::metadata(&out)?.len();
    // the old image's derived files (.map/.idx/.prelude of any layout) and images of other keys
    for e in std::fs::read_dir(&p.jc)?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let ours = n.starts_with(&plan.image_name);
        let derived = ours && n != plan.image_name;
        let other_image = n.ends_with(".aot") && !ours && n.len() > 40 && n.as_bytes()[32] == b'-';
        let other_derived = !ours && n.len() > 40 && n.as_bytes().get(32) == Some(&b'-') && n.contains(".aot.");
        if derived || other_image || other_derived {
            let _ = std::fs::remove_file(e.path());
        }
    }
    let img = Image { file: plan.image_name.clone(), size, code_size, chunks: files.len(), blocks: n[0], built_at: now() };
    man.image = Some(img.clone());
    Ok(img)
}
