//! The training hot set: block descriptors recorded by training boots (`<uuid>-<build>.<exe>.bin` files, any build),
//! with the number of profiles each block appears in (hotter = more processes ran it). Cached in `<aot>/hot.bin`,
//! rebuilt when the set of training files changes. Speed 31: also `<uuid>-<build>.<exe>.live` (descriptors only: what a
//! process on the image translated itself, libarm64jit X86CODE_A64_AOT_LIVE=1).
use anyhow::Result;
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

pub struct Hot {
    /// (descriptor, rank) sorted by descriptor; rank: see `rank` (higher = more worth a place in the shared image)
    pub blocks: Vec<(u64, u32)>,
    pub files: usize,
    /// Speed 31: descriptors from `.live` files (sorted): blocks processes on the image had to translate themselves.
    pub live: Vec<u64>,
    /// Speed 33: distinct sampled PCs (.cnt files) that weighed into the counts
    pub sampled: usize,
}

/// RAM 2 (JIT): a block's rank packs, from high to low bits: the number of distinct executables that ran it (bits
/// 24-31: a .bin and a .live of the same program count once, so repeated boots no longer inflate it), a flag for
/// blocks some process on the image translated itself (bit 23, .live) and sampled execution hits (bits 0-22,
/// X86CODE_A64_COUNT). Sharing decides first: a block in the image is one clean page-cache copy for every process
/// that runs it, a block left out is translated (time) and kept (private code + JIT metadata) by each of them.
/// Before, the flag was worth more than any sharing (LIVE_RANK = 1 << 20 added per block) and a full build that did
/// not fit left out whole libraries by summed rank per byte: the 2026-11-03 image had Foundation, libobjc,
/// libdispatch, CFNetwork and libsqlite3 out (every daemon translated them live) and UIKitCore, WebCore, VectorKit in.
pub const LIVE_RANK: u32 = 1 << 23;
const SAMPLE_MAX: u32 = LIVE_RANK - 1;
pub const fn rank(exes: u32, live: bool, samples: u32) -> u32 {
    (if exes > 255 { 255 } else { exes }) << 24 | if live { LIVE_RANK } else { 0 } | if samples > SAMPLE_MAX { SAMPLE_MAX } else { samples }
}
pub const fn is_live(r: u32) -> bool {
    r & LIVE_RANK != 0
}
pub const fn exes(r: u32) -> u32 {
    r >> 24
}

const PC_MASK: u64 = (1 << 56) - 1;

fn training_files(dirs: &[PathBuf], uuid: &str, until: Option<u64>) -> Vec<PathBuf> {
    use std::os::unix::fs::MetadataExt;
    let mut v = Vec::new();
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            let new = until.is_some_and(|u| e.metadata().map(|m| m.mtime() as u64 > u).unwrap_or(false));
            if n.starts_with(uuid) && n.ends_with(".bin") && !new {
                v.push(e.path());
            }
        }
    }
    v.sort();
    v
}

pub fn read_descs(p: &Path) -> Option<Vec<u64>> {
    let mut f = std::fs::File::open(p).ok()?;
    let mut h = [0u8; 160];
    f.read_exact(&mut h).ok()?;
    let start = match &h[..8] {
        b"A64JC06\0" => 160,
        b"A64JC05\0" => 152,
        b"A64CK01\0" => 168, // chunk: u64 code_off after the header
        _ => return None,
    };
    let n = u64::from_le_bytes(h[40..48].try_into().unwrap()) as usize;
    if n > 1 << 26 {
        return None;
    }
    f.seek(SeekFrom::Start(start)).ok()?;
    let mut raw = vec![0u8; n * 24];
    f.read_exact(&mut raw).ok()?;
    Some((0..n).map(|i| u64::from_le_bytes(raw[i * 24..i * 24 + 8].try_into().unwrap())).collect())
}

fn signature(files: &[PathBuf]) -> u64 {
    use std::os::unix::fs::MetadataExt;
    let mut h = crate::key::Fnv64::new();
    for f in files {
        h.str(&f.to_string_lossy());
        if let Ok(m) = std::fs::metadata(f) {
            h.u64(m.len());
            h.u64(m.mtime() as u64);
        }
    }
    h.finish()
}

fn live_descs(dirs: &[PathBuf], uuid: &str) -> Vec<u64> {
    let mut v = Vec::new();
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.starts_with(uuid) && n.ends_with(".live") {
                v.extend(read_descs(&e.path()).unwrap_or_default());
            }
        }
    }
    v.sort_unstable();
    v.dedup();
    v
}

/// Speed 33: sampled execution counts (libarm64jit X86CODE_A64_COUNT=1: `<uuid>-<build>.<exe>.cnt`, "A64CNT1\0",
/// u64 n, n x {u64 pc, u32 hits, u32 0}), summed by guest PC over every file.
fn sample_counts(dirs: &[PathBuf], uuid: &str) -> HashMap<u64, u64> {
    let mut m: HashMap<u64, u64> = HashMap::new();
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !(n.starts_with(uuid) && n.ends_with(".cnt")) {
                continue;
            }
            let Ok(b) = std::fs::read(e.path()) else { continue };
            if b.len() < 16 || &b[..8] != b"A64CNT1\0" {
                continue;
            }
            for r in b[16..].chunks_exact(16) {
                let pc = u64::from_le_bytes(r[..8].try_into().unwrap());
                *m.entry(pc).or_default() += u32::from_le_bytes(r[8..12].try_into().unwrap()) as u64;
            }
        }
    }
    m
}

/// Weight of one sampled hit (in rank's sample bits).
pub const SAMPLE_W: u32 = 1 << 10;

/// `<uuid>-<build>.<exe>.<ext>` -> exe (the key part has no dot)
fn exe_of(name: &str) -> Option<&str> {
    let i = name.find('.')?;
    let j = name.rfind('.')?;
    (j > i).then(|| &name[i + 1..j])
}

pub fn load(dirs: &[PathBuf], uuid: &str, cache_file: &Path, until: Option<u64>) -> Result<Hot> {
    let bins = training_files(dirs, uuid, until);
    let mut lives: Vec<PathBuf> = Vec::new();
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.starts_with(uuid) && n.ends_with(".live") {
                lives.push(e.path());
            }
        }
    }
    lives.sort();
    let mut all = bins.clone();
    all.extend(lives.iter().cloned());
    // "R2J1": the rank format (a hot.bin of the old per-file counts is never reused)
    let sig = signature(&all) ^ 0x5232_4a31_0000_0000;
    let cached = std::fs::read(cache_file).ok().and_then(|b| {
        if b.len() < 16 || u64::from_le_bytes(b[..8].try_into().unwrap()) != sig {
            return None;
        }
        let n = u64::from_le_bytes(b[8..16].try_into().unwrap()) as usize;
        (b.len() == 16 + n * 12).then(|| {
            (0..n)
                .map(|i| {
                    let o = 16 + i * 12;
                    (u64::from_le_bytes(b[o..o + 8].try_into().unwrap()), u32::from_le_bytes(b[o + 8..o + 12].try_into().unwrap()))
                })
                .collect::<Vec<(u64, u32)>>()
        })
    });
    let live = live_descs(dirs, uuid);
    let cnt = sample_counts(dirs, uuid);
    let blocks = match cached {
        Some(b) => b,
        None => {
            // descriptors per executable (its .bin files and its .live), then one count per executable
            let mut per_exe: HashMap<String, Vec<u64>> = HashMap::new();
            for f in &all {
                let n = f.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
                let exe = exe_of(&n).unwrap_or("?").to_string();
                if let Some(v) = read_descs(f) {
                    per_exe.entry(exe).or_default().extend(v);
                }
            }
            let mut m: HashMap<u64, u32> = HashMap::new();
            for (_, mut v) in per_exe {
                v.sort_unstable();
                v.dedup();
                for d in v {
                    *m.entry(d).or_default() += 1;
                }
            }
            let mut blocks: Vec<(u64, u32)> = m.into_iter().collect();
            blocks.sort_unstable();
            let mut out = Vec::with_capacity(16 + blocks.len() * 12);
            out.extend_from_slice(&sig.to_le_bytes());
            out.extend_from_slice(&(blocks.len() as u64).to_le_bytes());
            for (d, c) in &blocks {
                out.extend_from_slice(&d.to_le_bytes());
                out.extend_from_slice(&c.to_le_bytes());
            }
            if let Some(p) = cache_file.parent() {
                std::fs::create_dir_all(p)?;
            }
            std::fs::write(cache_file, out)?;
            blocks
        }
    };
    let blocks = blocks
        .into_iter()
        .map(|(d, n)| {
            let s = cnt.get(&(d & PC_MASK)).map(|&h| h.min((SAMPLE_MAX / SAMPLE_W) as u64) as u32 * SAMPLE_W).unwrap_or(0);
            (d, rank(n, live.binary_search(&d).is_ok(), s))
        })
        .collect();
    Ok(Hot { blocks, files: bins.len(), sampled: cnt.len(), live })
}

impl Hot {
    /// Hot descriptors whose PC is in the ranges, hottest first (then by PC).
    pub fn in_ranges(&self, ranges: &[(u64, u64)]) -> Vec<(u64, u32)> {
        // descriptors sort by PC first only when the FPCR bits are zero (the usual case); scan with the PC mask
        let mut v = Vec::new();
        for &(lo, hi) in ranges {
            let i = self.blocks.partition_point(|&(d, _)| d < lo);
            for &(d, c) in &self.blocks[i..] {
                if d >= hi {
                    break;
                }
                v.push((d, c));
            }
        }
        // blocks with FPCR bits set (rare): match by PC
        for &(d, c) in self.blocks.iter().rev() {
            if d <= PC_MASK {
                break;
            }
            let pc = d & PC_MASK;
            if ranges.iter().any(|&(lo, hi)| pc >= lo && pc < hi) {
                v.push((d, c));
            }
        }
        v.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
        v
    }
}
