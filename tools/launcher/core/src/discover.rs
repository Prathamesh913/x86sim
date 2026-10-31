//! Static discovery: every chunk source (one per dyld-cache dylib and per placed non-cache Mach-O) with its code
//! ranges at run-time addresses, its identity (Mach-O UUID, or size+mtime) and its function entries.
use crate::dyldcache::{hex, Cache};
use crate::macho::{function_entries, FileImage};
use anyhow::{Context, Result};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum Kind {
    /// a dylib of the dyld shared cache (system tier)
    Cache,
    /// a placed Mach-O of the runtime (system tier)
    Placed,
    /// a placed app executable from a device's Containers/ (app tier)
    App,
}

#[derive(Debug, Clone)]
pub struct Source {
    /// unique chunk name: the install name (cache) or the placement key (placed files)
    pub name: String,
    pub kind: Kind,
    /// identity of the library's bytes: Mach-O UUID, else size:mtime
    pub ident: String,
    /// run-time load address (cache address or placement slot)
    pub addr: u64,
    /// code ranges at run-time addresses
    pub ranges: Vec<(u64, u64)>,
    pub code_bytes: u64,
}

/// One line of placement.txt: "addr slot fsize mtime key".
#[derive(Debug, Clone)]
pub struct Placement {
    pub addr: u64,
    pub slot: u64,
    pub fsize: u64,
    pub mtime: u64,
    pub key: String,
}

pub fn read_placement(path: &Path) -> Vec<Placement> {
    let Ok(s) = std::fs::read_to_string(path) else { return vec![] };
    s.lines()
        .filter_map(|l| {
            let mut it = l.splitn(5, ' ');
            let addr = u64::from_str_radix(it.next()?, 16).ok()?;
            let slot = u64::from_str_radix(it.next()?, 16).ok()?;
            let fsize = it.next()?.parse().ok()?;
            let mtime = it.next()?.parse().ok()?;
            let key = it.next()?.trim().to_string();
            (!key.is_empty()).then_some(Placement { addr, slot, fsize, mtime, key })
        })
        .collect()
}

pub fn cache_sources(cache: &Cache) -> Vec<Source> {
    cache
        .images
        .iter()
        .filter_map(|img| {
            let h = cache.header(img)?;
            let ranges = h.code_ranges();
            let code_bytes = ranges.iter().map(|r| r.1 - r.0).sum();
            Some(Source {
                name: img.path.clone(),
                kind: Kind::Cache,
                ident: h.uuid.map(|u| hex(&u)).unwrap_or_else(|| format!("addr:{:x}", img.addr)),
                addr: img.addr,
                ranges,
                code_bytes,
            })
        })
        .collect()
}

/// Entries of a cache source (function starts, symbols, unwind info), run-time addresses.
pub fn cache_entries(cache: &Cache, src: &Source) -> Vec<u64> {
    cache.images.iter().find(|i| i.path == src.name).and_then(|i| cache.header(i)).map(|h| function_entries(&h, cache)).unwrap_or_default()
}

/// Resolve a placement key to the file: runtime keys under RuntimeRoot/, app keys ("Containers/...") under the
/// device's data dir.
pub fn placed_path(p: &Placement, runtime_root: &Path, device: Option<&Path>) -> Option<PathBuf> {
    if p.key.starts_with("Containers/") {
        device.map(|d| d.join("data").join(&p.key))
    } else {
        Some(runtime_root.join(&p.key))
    }
}

pub struct PlacedFile {
    pub source: Source,
    pub path: PathBuf,
}

/// A placed file as a chunk source (None if missing, changed since placement, or not arm64 Mach-O).
pub fn placed_source(p: &Placement, path: &Path) -> Result<Option<PlacedFile>> {
    let Ok(md) = std::fs::metadata(path) else { return Ok(None) };
    use std::os::unix::fs::MetadataExt;
    if md.len() != p.fsize || md.mtime() as u64 != p.mtime {
        return Ok(None); // libarm64jit would not place it either
    }
    let data = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    let Some(img) = FileImage::parse(&data) else { return Ok(None) };
    let base = img.header.base();
    let ranges: Vec<(u64, u64)> = img.header.code_ranges().iter().map(|&(lo, hi)| (p.addr + lo - base, p.addr + hi - base)).collect();
    let code_bytes = ranges.iter().map(|r| r.1 - r.0).sum();
    let ident = img.header.uuid.map(|u| hex(&u)).unwrap_or_else(|| format!("{}:{}", p.fsize, p.mtime));
    Ok(Some(PlacedFile {
        source: Source {
            name: p.key.clone(),
            kind: if p.key.starts_with("Containers/") { Kind::App } else { Kind::Placed },
            ident,
            addr: p.addr,
            ranges,
            code_bytes,
        },
        path: path.to_path_buf(),
    }))
}

pub fn placed_entries(p: &Placement, path: &Path) -> Vec<u64> {
    let Ok(data) = std::fs::read(path) else { return vec![] };
    let Some(img) = FileImage::parse(&data) else { return vec![] };
    let base = img.header.base();
    function_entries(&img.header, &img).into_iter().map(|a| p.addr + a - base).collect()
}
