//! x86code-mcp: a small MCP server (JSON-RPC 2.0 over stdio, one message per line) for the x86Code arm64 simulator:
//! feature toggles (list / get / set / preset), sim control (status / start / stop / restart to apply) and the AOT
//! image status. Uses x86code-core like the CLI and the GUI.
//!
//! Register it with an MCP client, e.g. `claude mcp add x86code -- /path/to/x86code-mcp`.
use serde_json::{json, Value};
use std::io::{BufRead, Write};
use std::path::PathBuf;
use x86code_core::{aot, features, sim};

fn tools() -> Value {
    let dev = json!({"type": "string", "description": "device dir (default: the configured device, var/device-a64)"});
    json!([
        {"name": "features_list", "description": "List the device's optional features: id, title, on/off, default, measured memory cost (MB) and whether it is paid every boot or only when used. Changes apply at the next boot.",
         "inputSchema": {"type": "object", "properties": {"device": dev}}},
        {"name": "feature_get", "description": "Whether one feature is on.",
         "inputSchema": {"type": "object", "properties": {"id": {"type": "string"}, "device": dev}, "required": ["id"]}},
        {"name": "feature_set", "description": "Turn a feature on or off (wallpaper, spotlight, keyboard, siri, maps, photos, files, widgets). Applies at the next boot: call sim_restart.",
         "inputSchema": {"type": "object", "properties": {"id": {"type": "string"}, "enabled": {"type": "boolean"}, "device": dev}, "required": ["id", "enabled"]}},
        {"name": "features_preset", "description": "Apply a preset: full (the default set), lean, minimal. Applies at the next boot.",
         "inputSchema": {"type": "object", "properties": {"name": {"type": "string", "enum": ["full", "lean", "minimal"]}, "device": dev}, "required": ["name"]}},
        {"name": "sim_status", "description": "Simulator status: booted, home screen reached, process count, CPU %, PSS and anon PSS (MB), simview.",
         "inputSchema": {"type": "object", "properties": {}}},
        {"name": "sim_devices", "description": "Devices (folder, label with phone model) and the phone models a new device can be.",
         "inputSchema": {"type": "object", "properties": {}}},
        {"name": "sim_add_device", "description": "Make a new device of a phone model (see sim_devices models), optional name and iOS version (ios, e.g. \"27.2\"; see sim_devices runtimes; default 27.0). Boot it with sim_start device=<path>.",
         "inputSchema": {"type": "object", "properties": {"model": {"type": "string"}, "name": {"type": "string"}, "ios": {"type": "string"}}, "required": ["model"]}},
        {"name": "sim_restart", "description": "Restart the simulator (stop, then boot with the device's current features and the AOT image). Returns at once; poll sim_status for home.",
         "inputSchema": {"type": "object", "properties": {"device": dev}}},
        {"name": "sim_start", "description": "Boot the simulator.", "inputSchema": {"type": "object", "properties": {"device": dev}}},
        {"name": "sim_stop", "description": "Stop the simulator.", "inputSchema": {"type": "object", "properties": {"device": dev}}},
        {"name": "aot_status", "description": "Pre-translated image status of a phone: chunks up to date / stale (with reasons) / missing.",
         "inputSchema": {"type": "object", "properties": {"device": dev}}}
    ])
}

fn call(name: &str, a: &Value) -> anyhow::Result<Value> {
    let jc = aot::Paths::default_jc();
    let p = aot::Paths::new(&jc);
    let cfg = p.load_config();
    let device = a["device"].as_str().map(PathBuf::from).or(cfg.device.clone()).ok_or_else(|| anyhow::anyhow!("no device"))?;
    Ok(match name {
        "features_list" => json!({"device": device, "preset": features::preset_of(&features::load(&device)), "features": features::list(&device)}),
        "feature_get" => {
            let id = a["id"].as_str().unwrap_or("");
            json!({"id": id, "enabled": features::load(&device).enabled(id)})
        }
        "feature_set" => {
            let id = a["id"].as_str().unwrap_or("");
            let on = a["enabled"].as_bool().ok_or_else(|| anyhow::anyhow!("enabled: boolean required"))?;
            features::set(&device, id, on)?;
            json!({"id": id, "enabled": on, "note": "applies at the next boot (sim_restart)"})
        }
        "features_preset" => {
            let n = a["name"].as_str().unwrap_or("");
            features::apply_preset(&device, n)?;
            json!({"preset": n, "features": features::list(&device), "note": "applies at the next boot (sim_restart)"})
        }
        "sim_status" => serde_json::to_value(sim::status(1000))?,
        "sim_devices" => json!({"devices": sim::devices().into_iter().map(|d| json!({"label": d.0, "path": d.1, "ios": sim::runtime_short(&sim::device_runtime(&d.1))})).collect::<Vec<_>>(),
                                "models": sim::models().into_iter().map(|m| m.0).collect::<Vec<_>>(),
                                "runtimes": sim::runtimes().into_iter().map(|r| r.0).collect::<Vec<_>>()}),
        "sim_add_device" => {
            let want = a["ios"].as_str().unwrap_or("27.0");
            let rt = sim::runtimes().into_iter().find(|r| r.0.starts_with(&format!("iOS {want} ")) || r.0.contains(want))
                .map(|r| r.1).ok_or_else(|| anyhow::anyhow!("no iOS {want} runtime (see sim_devices runtimes)"))?;
            json!({"device": sim::create_device(a["model"].as_str().unwrap_or(""), a["name"].as_str().unwrap_or(""), &rt)?})
        }
        "sim_restart" => json!({"restarting": true, "pid": sim::restart(&device)?}),
        "sim_start" => json!({"booting": true, "pid": sim::start(&device)?}),
        "sim_stop" => {
            sim::stop(&device)?;
            json!({"stopped": true})
        }
        "aot_status" => {
            // the phone's iOS version decides the image and its cache dir
            let rt = sim::device_runtime(&device);
            let p = aot::Paths::new(&sim::jc_for(&rt));
            let mut cfg = p.load_config();
            cfg.runtime_root = rt;
            cfg.device = Some(device.clone());
            let st = aot::plan(&p, &cfg)?.status(&p);
            let attention: Vec<_> = st.chunks.iter().filter(|c| c.state != "up-to-date").take(50).collect();
            json!({"image": st.image, "image_current": st.image_current, "total": st.total, "up_to_date": st.up_to_date,
                   "stale": st.stale, "missing": st.missing, "need_rebuild": attention})
        }
        _ => anyhow::bail!("unknown tool {name}"),
    })
}

fn main() {
    let stdin = std::io::stdin();
    let mut out = std::io::stdout();
    for line in stdin.lock().lines() {
        let Ok(line) = line else { break };
        let Ok(req) = serde_json::from_str::<Value>(&line) else { continue };
        let id = req.get("id").cloned();
        let method = req["method"].as_str().unwrap_or("");
        let result: Result<Value, (i64, String)> = match method {
            "initialize" => Ok(json!({
                "protocolVersion": req["params"]["protocolVersion"].as_str().unwrap_or("2025-06-18"),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "x86code", "version": env!("CARGO_PKG_VERSION")}
            })),
            "ping" => Ok(json!({})),
            "tools/list" => Ok(json!({"tools": tools()})),
            "tools/call" => {
                let name = req["params"]["name"].as_str().unwrap_or("");
                let args = req["params"].get("arguments").cloned().unwrap_or(json!({}));
                Ok(match call(name, &args) {
                    Ok(v) => json!({"content": [{"type": "text", "text": serde_json::to_string_pretty(&v).unwrap_or_default()}], "structuredContent": v}),
                    Err(e) => json!({"content": [{"type": "text", "text": format!("{e:#}")}], "isError": true}),
                })
            }
            m if m.starts_with("notifications/") => continue,
            _ => Err((-32601, format!("method not found: {method}"))),
        };
        let Some(id) = id else { continue };
        let resp = match result {
            Ok(r) => json!({"jsonrpc": "2.0", "id": id, "result": r}),
            Err((c, m)) => json!({"jsonrpc": "2.0", "id": id, "error": {"code": c, "message": m}}),
        };
        let _ = writeln!(out, "{resp}");
        let _ = out.flush();
    }
}
