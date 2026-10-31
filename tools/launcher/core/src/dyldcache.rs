//! The iOS simulator's arm64 dyld shared cache: the main file plus its subcaches (`.01`, ...), each with its own
//! mappings. Images (dylibs) are listed in the main file's header (imagesOffset / imagesCount at 0x1C0).
use crate::macho::{u32le, u64le, AddrSpace, Header};
use anyhow::{bail, Context, Result};
use memmap2::Mmap;
use std::fs::File;
use std::path::{Path, PathBuf};

pub const CACHE_NAME: &str = "dyld_sim_shared_cache_arm64";

struct Mapping {
    addr: u64,
    size: u64,
    file: usize,
    fileoff: u64,
}

pub struct Cache {
    pub uuid: [u8; 16],
    files: Vec<Mmap>,
    maps: Vec<Mapping>,
    pub images: Vec<CacheImage>,
}

#[derive(Debug, Clone)]
pub struct CacheImage {
    pub path: String,
    pub addr: u64,
}

impl Cache {
    pub fn cache_dir(runtime_root: &Path) -> PathBuf {
        runtime_root.join("System/Library/Caches/com.apple.dyld")
    }

    pub fn open(runtime_root: &Path) -> Result<Cache> {
        let dir = Self::cache_dir(runtime_root);
        let main = dir.join(CACHE_NAME);
        let mut paths = vec![main.clone()];
        let mut subs: Vec<PathBuf> = std::fs::read_dir(&dir)
            .with_context(|| format!("reading {}", dir.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| {
                p.file_name().and_then(|n| n.to_str()).is_some_and(|n| {
                    n.strip_prefix(CACHE_NAME).and_then(|s| s.strip_prefix('.')).is_some_and(|s| s.chars().all(|c| c.is_ascii_digit()) && !s.is_empty())
                })
            })
            .collect();
        subs.sort();
        paths.extend(subs);
        let mut files = Vec::new();
        let mut maps = Vec::new();
        for (i, p) in paths.iter().enumerate() {
            let f = File::open(p).with_context(|| format!("opening {}", p.display()))?;
            // SAFETY: read-only mapping of an immutable runtime file
            let m = unsafe { Mmap::map(&f)? };
            if m.len() < 0x200 || &m[..7] != b"dyld_v1" {
                bail!("{} is not a dyld cache", p.display());
            }
            let mo = u32le(&m, 0x10) as usize;
            let mc = u32le(&m, 0x14) as usize;
            for k in 0..mc {
                let e = mo + k * 32;
                maps.push(Mapping { addr: u64le(&m, e), size: u64le(&m, e + 8), fileoff: u64le(&m, e + 16), file: i });
            }
            files.push(m);
        }
        let m0 = &files[0];
        let uuid: [u8; 16] = m0[0x58..0x68].try_into().unwrap();
        let io = u32le(m0, 0x1c0) as usize;
        let ic = u32le(m0, 0x1c4) as usize;
        let mut images = Vec::with_capacity(ic);
        for k in 0..ic {
            let e = io + k * 32;
            let addr = u64le(m0, e);
            let po = u32le(m0, e + 24) as usize;
            let end = m0[po..].iter().position(|&c| c == 0).unwrap_or(0);
            images.push(CacheImage { path: String::from_utf8_lossy(&m0[po..po + end]).into_owned(), addr });
        }
        Ok(Cache { uuid, files, maps, images })
    }

    pub fn uuid_hex(&self) -> String {
        hex(&self.uuid)
    }

    pub fn header(&self, img: &CacheImage) -> Option<Header> {
        Header::parse(self.read(img.addr, 0x8000).or_else(|| self.read(img.addr, 0x1000))?)
    }
}

impl AddrSpace for Cache {
    fn read(&self, addr: u64, len: usize) -> Option<&[u8]> {
        for m in &self.maps {
            if addr >= m.addr && addr + len as u64 <= m.addr + m.size {
                let o = (m.fileoff + addr - m.addr) as usize;
                return self.files[m.file].get(o..o + len);
            }
        }
        None
    }
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
