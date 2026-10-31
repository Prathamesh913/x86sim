//! The training hot set: block descriptors recorded by training boots (`<uuid>-<build>.<exe>.bin` files, any build),
//! with the number of profiles each block appears in (hotter = more processes ran it). Cached in `<aot>/hot.bin`,
//! rebuilt when the set of training files changes.
use anyhow::Result;
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

pub struct Hot {
    /// (descriptor, profiles) sorted by descriptor
    pub blocks: Vec<(u64, u32)>,
    pub files: usize,
}

const PC_MASK: u64 = (1 << 56) - 1;

fn training_files(dirs: &[PathBuf], uuid: &str) -> Vec<PathBuf> {
    let mut v = Vec::new();
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.starts_with(uuid) && n.ends_with(".bin") {
                v.push(e.path());
            }
        }
    }
    v.sort();
    v
}

fn read_descs(p: &Path) -> Option<Vec<u64>> {
    let mut f = std::fs::File::open(p).ok()?;
    let mut h = [0u8; 160];
    f.read_exact(&mut h).ok()?;
    let start = match &h[..8] {
        b"A64JC06\0" => 160,
        b"A64JC05\0" => 152,
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

pub fn load(dirs: &[PathBuf], uuid: &str, cache_file: &Path) -> Result<Hot> {
    let files = training_files(dirs, uuid);
    let sig = signature(&files);
    if let Ok(b) = std::fs::read(cache_file) {
        if b.len() >= 16 && u64::from_le_bytes(b[..8].try_into().unwrap()) == sig {
            let n = u64::from_le_bytes(b[8..16].try_into().unwrap()) as usize;
            if b.len() == 16 + n * 12 {
                let blocks = (0..n)
                    .map(|i| {
                        let o = 16 + i * 12;
                        (u64::from_le_bytes(b[o..o + 8].try_into().unwrap()), u32::from_le_bytes(b[o + 8..o + 12].try_into().unwrap()))
                    })
                    .collect();
                return Ok(Hot { blocks, files: files.len() });
            }
        }
    }
    let mut m: HashMap<u64, u32> = HashMap::new();
    for f in &files {
        if let Some(v) = read_descs(f) {
            for d in v {
                *m.entry(d).or_default() += 1;
            }
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
    Ok(Hot { blocks, files: files.len() })
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
