//! Apps on a phone, as `xcrun simctl install|uninstall|launch|listapps`: the requests go to the phone's
//! com.x86sim.openurl job (host/openurl) through cli/x86sim (appinstall.sh / simctl.py), which installs through
//! InstallCoordination -> installd (LaunchServices registers the app, the home screen shows its icon) and launches
//! through FrontBoard. Used by the launcher ("Install app…") and the MCP tools app_install / app_uninstall /
//! app_launch / app_list.
use crate::aot::repo_root;
use crate::instance::Instance;
use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Command;

fn helper(inst: &Instance, args: &[&str]) -> Result<String> {
    let out = Command::new(repo_root().join("cli/x86sim"))
        .args(&args[..1])
        .arg(&inst.dir)
        .args(&args[1..])
        .current_dir(repo_root())
        .output()
        .context("running cli/x86sim")?;
    let text = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        bail!("{} {}: {}", args[0], args.get(1).unwrap_or(&""), if text.is_empty() { err.trim().to_string() } else { text });
    }
    Ok(text)
}

/// The bundle id in an .app's Info.plist (CFBundleIdentifier), XML or binary plist
pub fn bundle_id(app: &Path) -> Option<String> {
    let out = Command::new("python3")
        .args(["-I", "-c", "import plistlib,sys;print(plistlib.load(open(sys.argv[1],'rb'))['CFBundleIdentifier'])"])
        .arg(app.join("Info.plist"))
        .output()
        .ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
}

#[derive(Debug, Clone, Serialize)]
pub struct Installed {
    pub bundle_id: String,
    /// the helper's answer line (install <path> (<bundle id>): ...)
    pub answer: String,
}

/// Installs an .app bundle on a running phone (simctl install). Returns once installd answered.
pub fn install(inst: &Instance, app: &Path) -> Result<Installed> {
    let app: PathBuf = app.canonicalize().with_context(|| format!("{}", app.display()))?;
    anyhow::ensure!(app.join("Info.plist").is_file(), "{} is not an app bundle (no Info.plist)", app.display());
    let bundle_id = bundle_id(&app).unwrap_or_default();
    let answer = helper(inst, &["install", &app.to_string_lossy()])?;
    Ok(Installed { bundle_id, answer })
}

pub fn uninstall(inst: &Instance, bundle_id: &str) -> Result<String> {
    helper(inst, &["uninstall", bundle_id])
}

/// Opens an app (simctl launch); extra arguments go to the app
pub fn launch(inst: &Instance, bundle_id: &str, args: &[String]) -> Result<String> {
    let mut a = vec!["launch", bundle_id];
    a.extend(args.iter().map(|s| s.as_str()));
    helper(inst, &a)
}

/// Installed apps (simctl listapps): the helper's JSON answer
pub fn list(inst: &Instance) -> Result<String> {
    helper(inst, &["listapps"])
}
