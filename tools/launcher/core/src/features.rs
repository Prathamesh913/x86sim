//! Per-device feature toggles (`<device>/features.json`), defined in cli/libexec/arm64/features-def.json (shared with
//! boot-a64.sh, which applies them at the next boot).
use anyhow::{bail, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Feature {
    pub id: String,
    pub title: String,
    pub desc: String,
    #[serde(default)]
    pub default: bool,
    #[serde(default)]
    pub jobs: Vec<String>,
    #[serde(default)]
    pub add_jobs: Vec<String>,
    #[serde(default)]
    pub park: Vec<String>,
    #[serde(default)]
    pub chunks: Vec<String>,
    #[serde(default)]
    pub cost_mb: u64,
    #[serde(default)]
    pub when: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Defs {
    pub features: Vec<Feature>,
    pub presets: BTreeMap<String, BTreeMap<String, bool>>,
}

pub fn defs_path() -> PathBuf {
    crate::aot::repo_root().join("cli/libexec/arm64/features-def.json")
}

pub fn defs() -> Defs {
    static D: std::sync::OnceLock<Defs> = std::sync::OnceLock::new();
    D.get_or_init(|| {
        std::fs::read(defs_path())
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_else(|| serde_json::from_str(include_str!("../../../../cli/libexec/arm64/features-def.json")).expect("features-def.json"))
    })
    .clone()
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Profile {
    #[serde(default)]
    pub features: BTreeMap<String, bool>,
}

impl Profile {
    pub fn enabled(&self, id: &str) -> bool {
        self.features.get(id).copied().unwrap_or_else(|| defs().features.iter().find(|f| f.id == id).is_some_and(|f| f.default))
    }
}

pub fn profile_path(device: &Path) -> PathBuf {
    device.join("features.json")
}

pub fn load(device: &Path) -> Profile {
    std::fs::read(profile_path(device)).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

pub fn save(device: &Path, p: &Profile) -> Result<()> {
    crate::aot::write_json(&profile_path(device), p)
}

pub fn set(device: &Path, id: &str, on: bool) -> Result<Profile> {
    let d = defs();
    if !d.features.iter().any(|f| f.id == id) {
        bail!("unknown feature {id} (known: {})", d.features.iter().map(|f| f.id.as_str()).collect::<Vec<_>>().join(", "));
    }
    let mut p = load(device);
    p.features.insert(id.to_string(), on);
    save(device, &p)?;
    Ok(p)
}

pub fn apply_preset(device: &Path, name: &str) -> Result<Profile> {
    let d = defs();
    let Some(ps) = d.presets.get(name) else { bail!("unknown preset {name} (full, lean, minimal)") };
    let mut p = Profile::default();
    for f in &d.features {
        p.features.insert(f.id.clone(), ps.get(&f.id).copied().unwrap_or(f.default));
    }
    save(device, &p)?;
    Ok(p)
}

/// Which preset the profile matches, if any.
pub fn preset_of(p: &Profile) -> Option<String> {
    let d = defs();
    d.presets
        .iter()
        .find(|(_, ps)| d.features.iter().all(|f| p.enabled(&f.id) == ps.get(&f.id).copied().unwrap_or(f.default)))
        .map(|(n, _)| n.clone())
}

/// The feature whose code a chunk belongs to (placed executables / extensions only; shared frameworks stay).
pub fn feature_of_chunk(name: &str) -> Option<String> {
    if name.starts_with("/System/Library/") || name.starts_with("/usr/lib/") {
        return None; // cache dylibs are shared
    }
    defs().features.into_iter().find(|f| f.chunks.iter().any(|c| name.contains(c.as_str()))).map(|f| f.id)
}

#[derive(Debug, Clone, Serialize)]
pub struct FeatureState {
    pub id: String,
    pub title: String,
    pub desc: String,
    pub enabled: bool,
    pub default: bool,
    pub cost_mb: u64,
    pub when: String,
}

pub fn list(device: &Path) -> Vec<FeatureState> {
    let p = load(device);
    defs()
        .features
        .into_iter()
        .map(|f| FeatureState { enabled: p.enabled(&f.id), id: f.id, title: f.title, desc: f.desc, default: f.default, cost_mb: f.cost_mb, when: f.when })
        .collect()
}
