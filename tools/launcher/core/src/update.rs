//! Self-update for the AppImage: the newest GitHub release of x86code/x86code (pre-releases included while there are
//! only pre-releases), downloaded next to the running AppImage, checked against the release's SHA256SUMS and swapped in
//! with a rename (the running copy keeps working: its file stays open). The next start runs the new version; AppRun
//! copies its payload into the data dir once per version, so phones and data carry over.
//! Uses curl (a required program, see setup::checks) so the launcher needs no HTTP or TLS crates.
use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

pub const REPO: &str = "x86code/x86code";

#[derive(Debug, Clone)]
pub struct Release {
    /// "v0.1.0-pre4"
    pub tag: String,
    pub url: String,
    pub prerelease: bool,
    /// the x86_64 AppImage asset
    pub asset_name: String,
    pub asset_url: String,
    pub asset_size: u64,
    pub sums_url: Option<String>,
}

/// The running AppImage file ($APPIMAGE, set by the AppImage runtime); None in a checkout.
pub fn running_appimage() -> Option<PathBuf> {
    crate::setup::appimage()?;
    std::env::var_os("APPIMAGE").map(PathBuf::from).filter(|p| p.is_file())
}

/// "0.1.0-pre4" / "v0.1.0" -> sortable: (major, minor, patch, pre) with a final release after all its pre-releases.
/// git-describe suffixes ("-3-gabc1234") count as that version.
pub fn version_key(v: &str) -> Option<(u32, u32, u32, u32)> {
    let v = v.trim().trim_start_matches('v');
    let (core, rest) = v.split_once('-').map(|(a, b)| (a, Some(b))).unwrap_or((v, None));
    let n: Vec<u32> = core.split('.').map(|x| x.parse::<u32>().ok()).collect::<Option<_>>()?;
    let (a, b, c) = match n[..] {
        [a, b] => (a, b, 0),
        [a, b, c] => (a, b, c),
        _ => return None,
    };
    let pre = match rest {
        Some(r) if r.starts_with("pre") => r[3..].split(|ch: char| !ch.is_ascii_digit()).next()?.parse().ok()?,
        _ => u32::MAX,
    };
    Some((a, b, c, pre))
}

fn curl_json(url: &str) -> Result<serde_json::Value> {
    let out = Command::new("curl")
        .args(["-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", "-H", "User-Agent: x86code-launcher", url])
        .stdin(Stdio::null())
        .output()
        .context("running curl")?;
    if !out.status.success() {
        bail!("{}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(serde_json::from_slice(&out.stdout)?)
}

/// The newest published release with an AppImage (drafts skipped).
pub fn latest() -> Result<Option<Release>> {
    let v = curl_json(&format!("https://api.github.com/repos/{REPO}/releases?per_page=20"))?;
    let mut best: Option<(Release, (u32, u32, u32, u32))> = None;
    for r in v.as_array().into_iter().flatten() {
        if r["draft"].as_bool().unwrap_or(false) {
            continue;
        }
        let tag = r["tag_name"].as_str().unwrap_or("").to_string();
        let Some(k) = version_key(&tag) else { continue };
        let assets = r["assets"].as_array().cloned().unwrap_or_default();
        let Some(a) = assets.iter().find(|a| a["name"].as_str().is_some_and(|n| n.ends_with("-x86_64.AppImage"))) else { continue };
        let sums = assets.iter().find(|a| a["name"] == "SHA256SUMS").and_then(|a| a["browser_download_url"].as_str()).map(String::from);
        let rel = Release {
            tag,
            url: r["html_url"].as_str().unwrap_or("").into(),
            prerelease: r["prerelease"].as_bool().unwrap_or(false),
            asset_name: a["name"].as_str().unwrap_or("").into(),
            asset_url: a["browser_download_url"].as_str().unwrap_or("").into(),
            asset_size: a["size"].as_u64().unwrap_or(0),
            sums_url: sums,
        };
        if best.as_ref().is_none_or(|b| k > b.1) {
            best = Some((rel, k));
        }
    }
    Ok(best.map(|b| b.0))
}

/// A release newer than the running version, if any.
pub fn check() -> Result<Option<Release>> {
    let cur = crate::setup::version();
    let Some(ck) = version_key(&cur) else { return Ok(None) };
    Ok(latest()?.filter(|r| version_key(&r.tag).is_some_and(|k| k > ck)))
}

/// Downloads the release's AppImage next to `target`, checks its sha256 and renames it over `target`. `done` gets the
/// bytes written so far (for progress). Returns the installed path.
pub fn install(rel: &Release, target: &Path, done: Arc<AtomicU64>) -> Result<PathBuf> {
    let dir = target.parent().context("AppImage path has no folder")?;
    let part = dir.join(format!(".{}.part", rel.asset_name));
    let _ = std::fs::remove_file(&part);
    let mut child = Command::new("curl")
        .args(["-fsSL", "--retry", "3", "-H", "User-Agent: x86code-launcher", "-o"])
        .arg(&part)
        .arg(&rel.asset_url)
        .stdin(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .context("running curl")?;
    let status = loop {
        if let Some(s) = child.try_wait()? {
            break s;
        }
        done.store(std::fs::metadata(&part).map(|m| m.len()).unwrap_or(0), Ordering::Relaxed);
        std::thread::sleep(std::time::Duration::from_millis(250));
    };
    if !status.success() {
        let mut e = String::new();
        if let Some(mut s) = child.stderr.take() {
            use std::io::Read;
            let _ = s.read_to_string(&mut e);
        }
        let _ = std::fs::remove_file(&part);
        bail!("download failed: {}", e.trim());
    }
    done.store(std::fs::metadata(&part).map(|m| m.len()).unwrap_or(0), Ordering::Relaxed);
    // checksum (the release's SHA256SUMS); refuse an AppImage that doesn't match or can't be checked
    let check = (|| -> Result<()> {
        let url = rel.sums_url.as_deref().context("the release has no SHA256SUMS")?;
        let out = Command::new("curl").args(["-fsSL", "--max-time", "30", url]).stdin(Stdio::null()).output()?;
        anyhow::ensure!(out.status.success(), "couldn't fetch SHA256SUMS");
        let sums = String::from_utf8_lossy(&out.stdout);
        let want = sums.lines().find_map(|l| l.split_once("  ").filter(|(_, n)| n.trim() == rel.asset_name).map(|(h, _)| h.to_string())).context("SHA256SUMS doesn't list the AppImage")?;
        let got = Command::new("sha256sum").arg(&part).output()?;
        let got = String::from_utf8_lossy(&got.stdout).split_whitespace().next().unwrap_or("").to_string();
        anyhow::ensure!(got == want, "checksum mismatch (got {got}, expected {want})");
        Ok(())
    })();
    if let Err(e) = check {
        let _ = std::fs::remove_file(&part);
        return Err(e);
    }
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&part, std::fs::Permissions::from_mode(0o755))?;
    // the file keeps its name when it is version-less ("x86code.AppImage"); a versioned name follows the release
    let name = target.file_name().and_then(|n| n.to_str()).unwrap_or("");
    let dest = if version_key(name.trim_start_matches("x86code-").trim_end_matches("-x86_64.AppImage")).is_some() { dir.join(&rel.asset_name) } else { target.to_path_buf() };
    std::fs::rename(&part, &dest).with_context(|| format!("replacing {}", dest.display()))?;
    if dest != target {
        let _ = std::fs::remove_file(target);
    }
    Ok(dest)
}

/// Starts the given AppImage detached (the caller then quits).
pub fn relaunch(app: &Path) -> Result<()> {
    Command::new(app).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().with_context(|| format!("starting {}", app.display()))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::version_key;
    #[test]
    fn order() {
        assert!(version_key("v0.1.0-pre4") > version_key("0.1.0-pre2"));
        assert!(version_key("v0.1.0") > version_key("0.1.0-pre9"));
        assert!(version_key("v0.2.0-pre1") > version_key("0.1.0"));
        assert_eq!(version_key("0.1.0-pre2-3-gabc1234"), version_key("v0.1.0-pre2"));
        assert_eq!(version_key("0.1.0 (checkout)"), None);
    }
}
