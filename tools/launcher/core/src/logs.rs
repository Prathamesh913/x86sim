//! Live log tail over a phone's log dirs and state files: first sight shows the last 2 KB of a file, then only what
//! is appended (a truncated file starts over). Lines are kept in a bounded buffer, newest last.
use std::collections::{HashMap, VecDeque};
use std::io::{Read, Seek, SeekFrom};
use std::path::PathBuf;

#[derive(Clone, Debug)]
pub struct Line {
    pub source: String,
    pub text: String,
}

pub struct Tail {
    pub dirs: Vec<PathBuf>,
    pub files: Vec<PathBuf>,
    offs: HashMap<PathBuf, u64>,
    pub lines: VecDeque<Line>,
    pub sources: Vec<String>,
    pub cap: usize,
    pub generation: u64,
}

fn tag(p: &PathBuf) -> String {
    let n = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
    let n = n.strip_suffix(".log").unwrap_or(&n).to_string();
    n.strip_prefix("com.apple.").unwrap_or(&n).to_string()
}

impl Tail {
    pub fn new(dirs: Vec<PathBuf>, files: Vec<PathBuf>) -> Tail {
        Tail { dirs, files, offs: HashMap::new(), lines: VecDeque::new(), sources: vec![], cap: 6000, generation: 0 }
    }

    /// Reads what is new; returns the number of lines added.
    pub fn poll(&mut self) -> usize {
        let mut paths: Vec<PathBuf> = self.files.clone();
        for d in &self.dirs {
            for e in std::fs::read_dir(d).into_iter().flatten().flatten() {
                let p = e.path();
                if p.extension().is_some_and(|x| x == "log" || x == "out" || x == "txt") {
                    paths.push(p);
                }
            }
        }
        let mut added = 0;
        for p in paths {
            let Ok(md) = std::fs::metadata(&p) else { continue };
            let size = md.len();
            let mut o = match self.offs.get(&p) {
                None => size.saturating_sub(2048),
                Some(o) if size < *o => 0,
                Some(o) => *o,
            };
            if size > o {
                if let Ok(mut f) = std::fs::File::open(&p) {
                    let _ = f.seek(SeekFrom::Start(o));
                    let mut buf = vec![0u8; ((size - o) as usize).min(1 << 16)];
                    if let Ok(n) = f.read(&mut buf) {
                        buf.truncate(n);
                        o += n as u64;
                        let t = tag(&p);
                        if !self.sources.contains(&t) {
                            self.sources.push(t.clone());
                            self.sources.sort();
                        }
                        let s = String::from_utf8_lossy(&buf);
                        let mut it = s.lines().peekable();
                        if self.offs.get(&p).is_none() {
                            it.next(); // a partial first line of the tail
                        }
                        for l in it {
                            if l.trim().is_empty() {
                                continue;
                            }
                            self.lines.push_back(Line { source: t.clone(), text: l.to_string() });
                            added += 1;
                        }
                    }
                }
            }
            self.offs.insert(p, o);
        }
        while self.lines.len() > self.cap {
            self.lines.pop_front();
        }
        if added > 0 {
            self.generation += 1;
        }
        added
    }
}
