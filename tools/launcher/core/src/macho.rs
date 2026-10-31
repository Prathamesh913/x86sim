//! Minimal arm64 Mach-O reading: segments, code sections, LC_UUID, LC_FUNCTION_STARTS, LC_SYMTAB, unwind info.
//! Works on a standalone file (fat or thin) and on an image inside the dyld shared cache, where the load commands'
//! file offsets are resolved through the cache's mappings (see `dyldcache`).

pub const MH_MAGIC_64: u32 = 0xfeed_facf;
pub const CPU_TYPE_ARM64: u32 = 0x0100_000c;
const LC_SEGMENT_64: u32 = 0x19;
const LC_SYMTAB: u32 = 0x2;
const LC_UUID: u32 = 0x1b;
const LC_FUNCTION_STARTS: u32 = 0x26;
const S_ATTR_PURE_INSTRUCTIONS: u32 = 0x8000_0000;
const S_ATTR_SOME_INSTRUCTIONS: u32 = 0x0000_0400;

pub fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap())
}
pub fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
pub fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

#[derive(Debug, Clone, Default)]
pub struct Segment {
    pub name: String,
    pub vmaddr: u64,
    pub vmsize: u64,
    pub fileoff: u64,
    pub filesize: u64,
}

#[derive(Debug, Clone, Default)]
pub struct Section {
    pub segname: String,
    pub name: String,
    pub addr: u64,
    pub size: u64,
    pub offset: u32,
    pub flags: u32,
}

impl Section {
    pub fn is_code(&self) -> bool {
        self.flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS) != 0
    }
}

#[derive(Debug, Clone, Default)]
pub struct Header {
    pub segments: Vec<Segment>,
    pub sections: Vec<Section>,
    pub uuid: Option<[u8; 16]>,
    /// (dataoff, datasize) of LC_FUNCTION_STARTS
    pub function_starts: Option<(u32, u32)>,
    /// (symoff, nsyms, stroff, strsize)
    pub symtab: Option<(u32, u32, u32, u32)>,
    /// install names of LC_LOAD_DYLIB / weak / reexport / lazy / upward
    pub dylibs: Vec<String>,
}

impl Header {
    /// Parse the load commands of a thin arm64 Mach-O header at the start of `b`.
    pub fn parse(b: &[u8]) -> Option<Header> {
        if b.len() < 32 || u32le(b, 0) != MH_MAGIC_64 || u32le(b, 4) != CPU_TYPE_ARM64 {
            return None;
        }
        let ncmds = u32le(b, 16) as usize;
        let mut h = Header::default();
        let mut off = 32usize;
        for _ in 0..ncmds {
            if off + 8 > b.len() {
                break;
            }
            let cmd = u32le(b, off);
            let cs = u32le(b, off + 4) as usize;
            if cs < 8 || off + cs > b.len() {
                break;
            }
            match cmd {
                LC_SEGMENT_64 => {
                    let name = cstr(&b[off + 8..off + 24]);
                    h.segments.push(Segment {
                        name: name.clone(),
                        vmaddr: u64le(b, off + 24),
                        vmsize: u64le(b, off + 32),
                        fileoff: u64le(b, off + 40),
                        filesize: u64le(b, off + 48),
                    });
                    let nsects = u32le(b, off + 64) as usize;
                    for i in 0..nsects {
                        let s = off + 72 + i * 80;
                        if s + 80 > off + cs {
                            break;
                        }
                        h.sections.push(Section {
                            name: cstr(&b[s..s + 16]),
                            segname: cstr(&b[s + 16..s + 32]),
                            addr: u64le(b, s + 32),
                            size: u64le(b, s + 40),
                            offset: u32le(b, s + 48),
                            flags: u32le(b, s + 64),
                        });
                    }
                }
                0xc | 0x8000_0018 | 0x8000_001f | 0x20 | 0x8000_0023 if cs >= 24 => {
                    let no = u32le(b, off + 8) as usize;
                    if no < cs {
                        h.dylibs.push(cstr(&b[off + no..off + cs]));
                    }
                }
                LC_UUID if cs >= 24 => h.uuid = Some(b[off + 8..off + 24].try_into().unwrap()),
                LC_FUNCTION_STARTS => h.function_starts = Some((u32le(b, off + 8), u32le(b, off + 12))),
                LC_SYMTAB => h.symtab = Some((u32le(b, off + 8), u32le(b, off + 12), u32le(b, off + 16), u32le(b, off + 20))),
                _ => {}
            }
            off += cs;
        }
        Some(h)
    }

    /// The image's base (lowest mapped segment, i.e. __TEXT's vmaddr).
    pub fn base(&self) -> u64 {
        self.segments.iter().filter(|s| s.filesize > 0 || s.name != "__PAGEZERO").map(|s| s.vmaddr).min().unwrap_or(0)
    }

    pub fn text_vmaddr(&self) -> u64 {
        self.segments.iter().find(|s| s.name == "__TEXT").map(|s| s.vmaddr).unwrap_or_else(|| self.base())
    }

    /// Code ranges (sections with instruction attributes), [lo, hi) in the header's own address space.
    pub fn code_ranges(&self) -> Vec<(u64, u64)> {
        let mut v: Vec<(u64, u64)> = self.sections.iter().filter(|s| s.is_code() && s.size > 0).map(|s| (s.addr, s.addr + s.size)).collect();
        v.sort();
        v
    }

    /// Map a __LINKEDIT file offset (as stored in load commands) to a VM address.
    pub fn linkedit_addr(&self, fileoff: u64) -> Option<u64> {
        let le = self.segments.iter().find(|s| s.name == "__LINKEDIT")?;
        if fileoff < le.fileoff || fileoff >= le.fileoff + le.filesize.max(le.vmsize) {
            return None;
        }
        Some(le.vmaddr + (fileoff - le.fileoff))
    }
}

fn cstr(b: &[u8]) -> String {
    let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..n]).into_owned()
}

/// The arm64 slice of a fat file (offset, size), or the whole file if thin arm64.
pub fn arm64_slice(b: &[u8]) -> Option<(usize, usize)> {
    if b.len() >= 8 && b[..4] == [0xca, 0xfe, 0xba, 0xbe] {
        let n = u32::from_be_bytes(b[4..8].try_into().unwrap()) as usize;
        for i in 0..n {
            let e = 8 + 20 * i;
            if e + 20 > b.len() {
                break;
            }
            let ct = u32::from_be_bytes(b[e..e + 4].try_into().unwrap());
            if ct == CPU_TYPE_ARM64 {
                let o = u32::from_be_bytes(b[e + 8..e + 12].try_into().unwrap()) as usize;
                let s = u32::from_be_bytes(b[e + 12..e + 16].try_into().unwrap()) as usize;
                return (o + s <= b.len()).then_some((o, s));
            }
        }
        return None;
    }
    (b.len() >= 32 && u32le(b, 0) == MH_MAGIC_64 && u32le(b, 4) == CPU_TYPE_ARM64).then_some((0, b.len()))
}

/// Read bytes at a VM address of the image (a closure resolves addresses: cache mappings or file segments).
pub trait AddrSpace {
    fn read(&self, addr: u64, len: usize) -> Option<&[u8]>;
}

fn uleb(b: &[u8], p: &mut usize) -> Option<u64> {
    let (mut v, mut s) = (0u64, 0u32);
    loop {
        let c = *b.get(*p)?;
        *p += 1;
        v |= ((c & 0x7f) as u64) << s;
        if c & 0x80 == 0 {
            return Some(v);
        }
        s += 7;
        if s > 63 {
            return None;
        }
    }
}

/// Function entry points from LC_FUNCTION_STARTS, the symbol table (defined section symbols) and __unwind_info,
/// in the header's address space, restricted to its code ranges.
pub fn function_entries(h: &Header, mem: &dyn AddrSpace) -> Vec<u64> {
    let ranges = h.code_ranges();
    let in_code = |a: u64| a & 3 == 0 && ranges.iter().any(|&(lo, hi)| a >= lo && a + 4 <= hi);
    let mut out = Vec::new();
    let text = h.text_vmaddr();
    if let Some((off, size)) = h.function_starts {
        if let Some(b) = h.linkedit_addr(off as u64).and_then(|a| mem.read(a, size as usize)) {
            let (mut p, mut addr) = (0usize, text);
            while p < b.len() {
                match uleb(b, &mut p) {
                    Some(0) | None => break,
                    Some(d) => {
                        addr = addr.wrapping_add(d);
                        out.push(addr);
                    }
                }
            }
        }
    }
    if let Some((symoff, nsyms, _, _)) = h.symtab {
        if let Some(b) = h.linkedit_addr(symoff as u64).and_then(|a| mem.read(a, nsyms as usize * 16)) {
            for i in 0..nsyms as usize {
                let e = i * 16;
                let ty = b[e + 4];
                if ty & 0xe0 == 0 && ty & 0x0e == 0x0e {
                    out.push(u64le(b, e + 8));
                }
            }
        }
    }
    if let Some(s) = h.sections.iter().find(|s| s.segname == "__TEXT" && s.name == "__unwind_info") {
        if let Some(b) = mem.read(s.addr, s.size as usize) {
            unwind_entries(b, text, &mut out);
        }
    }
    out.retain(|&a| in_code(a));
    out.sort_unstable();
    out.dedup();
    out
}

fn unwind_entries(b: &[u8], base: u64, out: &mut Vec<u64>) {
    if b.len() < 28 || u32le(b, 0) != 1 {
        return;
    }
    let idx_off = u32le(b, 20) as usize;
    let idx_n = u32le(b, 24) as usize;
    for i in 0..idx_n {
        let e = idx_off + i * 12;
        if e + 12 > b.len() {
            return;
        }
        let func = u32le(b, e) as u64;
        let page = u32le(b, e + 4) as usize;
        if page == 0 || page + 8 > b.len() {
            continue;
        }
        out.push(base + func);
        match u32le(b, page) {
            2 => {
                let eo = u16le(b, page + 4) as usize;
                let n = u16le(b, page + 6) as usize;
                for k in 0..n {
                    let q = page + eo + k * 8;
                    if q + 8 > b.len() {
                        break;
                    }
                    out.push(base + u32le(b, q) as u64);
                }
            }
            3 => {
                let eo = u16le(b, page + 4) as usize;
                let n = u16le(b, page + 6) as usize;
                for k in 0..n {
                    let q = page + eo + k * 4;
                    if q + 4 > b.len() {
                        break;
                    }
                    out.push(base + func + (u32le(b, q) & 0x00ff_ffff) as u64);
                }
            }
            _ => {}
        }
    }
}

/// A standalone Mach-O file's arm64 slice, addressed by its own vmaddrs.
pub struct FileImage<'a> {
    pub slice: &'a [u8],
    pub header: Header,
}

impl<'a> FileImage<'a> {
    pub fn parse(file: &'a [u8]) -> Option<FileImage<'a>> {
        let (o, s) = arm64_slice(file)?;
        let slice = &file[o..o + s];
        let header = Header::parse(slice)?;
        Some(FileImage { slice, header })
    }
}

impl AddrSpace for FileImage<'_> {
    fn read(&self, addr: u64, len: usize) -> Option<&[u8]> {
        for s in &self.header.segments {
            if addr >= s.vmaddr && addr + len as u64 <= s.vmaddr + s.filesize {
                let o = (s.fileoff + addr - s.vmaddr) as usize;
                return self.slice.get(o..o + len);
            }
        }
        None
    }
}
