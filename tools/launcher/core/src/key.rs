//! Cache keys. Mirrors linux/arm64jit/jitcache.cpp: the image is `<cache uuid>-<build>.aot` with
//! build = CODEGEN_VERSION | code-shaping flags << 16 | FNV-1a(placement.txt) << 32. A chunk's key covers only what its
//! translated code depends on: the library's identity (UUID or size+mtime), its load address, CODEGEN_VERSION and the
//! flags. libarm64jit's build-id is not part of either: a translator rebuild that leaves codegen alone keeps
//! everything.
use std::path::Path;

/// Must equal kCodegenVersion in linux/arm64jit/jitcache.cpp (the builder reports its value; a mismatch is an error).
pub const CODEGEN_VERSION: u64 = 0x15;
/// inline objc (bit 17) and the Apple timebase (bit 18), the defaults; fuse_svc (bit 16) and the strict exclusive
/// monitor (bit 19) are off.
pub const FLAGS: u64 = 0x6 << 16;

pub struct Fnv64(u64);
impl Fnv64 {
    pub fn new() -> Self {
        Fnv64(0xcbf2_9ce4_8422_2325)
    }
    pub fn bytes(&mut self, b: &[u8]) {
        for &c in b {
            self.0 = (self.0 ^ c as u64).wrapping_mul(0x100_0000_01b3);
        }
    }
    pub fn str(&mut self, s: &str) {
        self.bytes(s.as_bytes());
        self.bytes(&[0]);
    }
    pub fn u64(&mut self, v: u64) {
        self.bytes(&v.to_le_bytes());
    }
    pub fn finish(&self) -> u64 {
        self.0
    }
}

impl Default for Fnv64 {
    fn default() -> Self {
        Self::new()
    }
}

/// libarm64jit's placement_load hash: FNV-1a (32-bit) over every byte of placement.txt; 0 without a usable table.
pub fn placement_hash(path: &Path) -> u32 {
    let Ok(b) = std::fs::read(path) else { return 0 };
    if crate::discover::read_placement(path).is_empty() {
        return 0;
    }
    let mut h: u32 = 2_166_136_261;
    for &c in &b {
        h = (h ^ c as u32).wrapping_mul(16_777_619);
    }
    h
}

pub fn build_id(placement: &Path) -> u64 {
    CODEGEN_VERSION | FLAGS | (placement_hash(placement) as u64) << 32
}

pub fn image_name(cache_uuid: &str, build: u64) -> String {
    format!("{cache_uuid}-{build:016x}.aot")
}

pub fn chunk_key(name: &str, ident: &str, addr: u64) -> String {
    let mut h = Fnv64::new();
    h.str(name);
    h.str(ident);
    h.u64(addr);
    h.u64(CODEGEN_VERSION);
    h.u64(FLAGS);
    format!("{:016x}", h.finish())
}

pub fn name_id(name: &str) -> String {
    let mut h = Fnv64::new();
    h.str(name);
    let base = name.rsplit('/').next().unwrap_or(name);
    let clean: String = base.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '_' }).take(40).collect();
    format!("{clean}.{:08x}", h.finish() as u32)
}
