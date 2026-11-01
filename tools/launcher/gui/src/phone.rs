//! One phone: its screen (touch + keyboard) with Home/Lock, and the fold-out panel: Overview, Features, Logs,
//! Performance, Advanced.
use crate::theme::*;
use crate::{viewer, App, View};
use eframe::egui;
use std::time::{Duration, Instant};
use x86code_core::{features, instance, instance::Phase, logs};

impl App {
    pub fn phone_view(&mut self, ctx: &egui::Context, id: &str) {
        let Some(inst) = self.inst(id).cloned() else {
            self.view = View::Home;
            return;
        };
        let st = self.status.get(id).cloned();
        let phase = st.as_ref().map(|s| s.phase).unwrap_or(Phase::Off);
        if self.settings.as_ref().map(|s| s.0.as_str()) != Some(id) {
            self.settings = Some((id.to_string(), instance::load_settings(&inst.dir)));
        }

        egui::TopBottomPanel::top("phone-head").frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin { left: 20, right: 20, top: 12, bottom: 10 })).show(ctx, |ui| {
            ui.horizontal(|ui| {
                if ui.add(small("←  Phones")).clicked() {
                    self.view = View::Home;
                }
                ui.add_space(6.0);
                ui.label(egui::RichText::new(&inst.name).size(19.0).strong());
                ui.label(muted(format!("{} · {}", inst.model, inst.ios)));
                match phase {
                    Phase::Running => pill(ui, "Running", GREEN),
                    Phase::Starting => pill(ui, "Starting…", ORANGE),
                    Phase::Stopping => pill(ui, "Stopping…", ORANGE),
                    Phase::Off if !inst.runtime_present => pill(ui, "iOS not downloaded", RED),
                    Phase::Off => pill(ui, "Off", MUTED),
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    match phase {
                        Phase::Off if !inst.runtime_present => {
                            if ui.add(primary("Download iOS")).clicked() {
                                self.view = View::Ios;
                            }
                        }
                        Phase::Off => {
                            if ui.add(primary("▶  Start")).clicked() {
                                self.start(id);
                            }
                        }
                        Phase::Stopping => {
                            ui.add_enabled(false, secondary("Stopping…"));
                        }
                        _ => {
                            if ui.add(danger("■  Stop")).clicked() {
                                self.stop(id);
                            }
                            if ui.add(secondary("⟳  Restart")).clicked() {
                                self.restart(id);
                            }
                        }
                    }
                    if !self.message.is_empty() && self.message_at.elapsed() < Duration::from_secs(6) {
                        ui.label(muted(&self.message).size(12.5));
                    }
                });
            });
        });

        egui::SidePanel::right("phone-side")
            .resizable(true)
            .default_width(400.0)
            .width_range(320.0..=720.0)
            .frame(egui::Frame::new().fill(RAIL).inner_margin(egui::Margin { left: 14, right: 14, top: 12, bottom: 12 }).stroke(egui::Stroke::new(1.0_f32, BORDER)))
            .show(ctx, |ui| {
                egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                    self.side_panel(ui, &inst, st.as_ref());
                });
            });

        egui::CentralPanel::default().frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::symmetric(20, 10))).show(ctx, |ui| {
            let live = matches!(phase, Phase::Running | Phase::Starting);
            let placeholder = match phase {
                Phase::Off if !inst.runtime_present => format!("{} is not downloaded", inst.ios),
                Phase::Off => "Off".into(),
                Phase::Starting => "Starting…".into(),
                Phase::Stopping => "Stopping…".into(),
                Phase::Running => "Waiting for the screen…".into(),
            };
            let avail = ui.available_size() - egui::vec2(0.0, 44.0);
            let (fps, seq) = {
                let v = self.viewer(id);
                (v.as_ref().map(|v| v.fps()).unwrap_or(0), v.as_ref().map(|v| v.shared.seq.load(std::sync::atomic::Ordering::Relaxed)).unwrap_or(0))
            };
            ui.vertical_centered(|ui| {
                if let Some(v) = self.viewer(id) {
                    v.show(ui, avail, live, &placeholder, true, viewer::FULL);
                }
                ui.add_space(6.0);
                ui.horizontal(|ui| {
                    let total = 3.0 * 90.0 + 2.0 * 8.0;
                    ui.add_space(((ui.available_width() - total) / 2.0).max(0.0));
                    let on = live;
                    if ui.add_enabled(on, secondary("○  Home").min_size(egui::vec2(90.0, 30.0))).on_hover_text("F1").clicked() {
                        if let Some(v) = self.viewer(id) {
                            v.press("home");
                        }
                    }
                    if ui.add_enabled(on, secondary("⏻  Lock").min_size(egui::vec2(90.0, 30.0))).on_hover_text("F2").clicked() {
                        if let Some(v) = self.viewer(id) {
                            v.press("lock");
                        }
                    }
                    let hint = if self.viewers.get(id).is_some_and(|v| v.focused) { "Keyboard goes to the phone" } else { "Click the screen to type" };
                    ui.add_enabled(false, secondary(if live { hint } else { "" }).min_size(egui::vec2(90.0, 30.0)).frame(false));
                });
                if live && seq > 0 {
                    ui.label(muted(format!("{fps} fps · drag from an edge for system gestures · Ctrl+drag pinch · Shift+drag two-finger scroll")).size(11.0));
                }
            });
        });
    }

    fn side_panel(&mut self, ui: &mut egui::Ui, inst: &instance::Instance, st: Option<&instance::Status>) {
        let phase = st.map(|s| s.phase).unwrap_or(Phase::Off);
        let id = inst.id.clone();
        // ---- Overview (always open)
        ui.label(egui::RichText::new("Overview").size(14.5).strong());
        ui.add_space(6.0);
        let (state, col) = match phase {
            Phase::Running => ("Running", GREEN),
            Phase::Starting => ("Starting…", ORANGE),
            Phase::Stopping => ("Stopping…", ORANGE),
            Phase::Off if !inst.runtime_present => ("Needs iOS", RED),
            Phase::Off => ("Off", MUTED),
        };
        let on = phase != Phase::Off;
        stat_row(ui, vec![
            ("STATUS", state.into(), col),
            ("MEMORY", if on { fmt_mb(st.map(|s| s.pss_mb).unwrap_or(0.0)) } else { "–".into() }, TEXT),
            ("CPU", if on { format!("{:.0}%", st.map(|s| s.cpu_pct).unwrap_or(0.0)) } else { "–".into() }, TEXT),
        ]);
        ui.add_space(8.0);
        egui::Grid::new("ov").num_columns(2).spacing([14.0, 5.0]).show(ui, |ui| {
            ui.label(muted("Model").size(12.5));
            ui.label(egui::RichText::new(&inst.model).size(12.5));
            ui.end_row();
            ui.label(muted("iOS").size(12.5));
            ui.label(egui::RichText::new(if inst.runtime_present { inst.ios.clone() } else { format!("{} (not downloaded)", inst.ios) }).size(12.5).color(if inst.runtime_present { TEXT } else { RED }));
            ui.end_row();
            ui.label(muted("Screen").size(12.5));
            ui.label(egui::RichText::new(format!("{} × {} @{:.0}x", inst.screen.0, inst.screen.1, inst.scale)).size(12.5));
            ui.end_row();
            if let Some(s) = st {
                if let Some(u) = s.uptime_s {
                    ui.label(muted("Up for").size(12.5));
                    ui.label(egui::RichText::new(fmt_s(u)).size(12.5));
                    ui.end_row();
                }
                if on {
                    ui.label(muted("Processes").size(12.5));
                    ui.label(egui::RichText::new(format!("{}{}", s.procs.len(), if s.mtlvk { " · GPU helper up" } else { "" })).size(12.5));
                    ui.end_row();
                }
            }
            let prof = features::load(&inst.dir);
            let off: Vec<String> = features::defs().features.iter().filter(|f| !prof.enabled(&f.id)).map(|f| f.title.clone()).collect();
            ui.label(muted("Features").size(12.5));
            ui.label(egui::RichText::new(if off.is_empty() { "Everything on".to_string() } else { format!("Off: {}", off.join(", ")) }).size(12.5));
            ui.end_row();
        });
        ui.add_space(10.0);
        hline(ui);
        ui.add_space(4.0);

        // ---- Features
        let prof = features::load(&inst.dir);
        let preset = features::preset_of(&prof).map(|p| match p.as_str() { "full" => "Full", "lean" => "Lean", "minimal" => "Minimal", _ => "Custom" }).unwrap_or("Custom");
        let open = self.open.contains("features");
        if fold(ui, "Features", preset, open) != open {
            if open { self.open.remove("features"); } else { self.open.insert("features"); }
        }
        if self.open.contains("features") {
            self.features_section(ui, inst, phase);
        }
        hline(ui);
        ui.add_space(4.0);

        // ---- Logs
        let open = self.open.contains("logs");
        let nlines = self.logs.as_ref().filter(|l| l.0 == id).map(|l| l.1.lines.len()).unwrap_or(0);
        if fold(ui, "Logs", &if open && nlines > 0 { format!("{nlines} lines") } else { String::new() }, open) != open {
            if open { self.open.remove("logs"); } else { self.open.insert("logs"); }
        }
        if self.open.contains("logs") {
            self.logs_section(ui, inst, st);
        } else {
            self.logs = None;
        }
        hline(ui);
        ui.add_space(4.0);

        // ---- Performance
        let open = self.open.contains("perf");
        let summary = if on { format!("{} · {:.0}% CPU", fmt_mb(st.map(|s| s.pss_mb).unwrap_or(0.0)), st.map(|s| s.cpu_pct).unwrap_or(0.0)) } else { String::new() };
        if fold(ui, "Performance", &summary, open) != open {
            if open { self.open.remove("perf"); } else { self.open.insert("perf"); }
        }
        if self.open.contains("perf") {
            self.perf_section(ui, st);
        }
        hline(ui);
        ui.add_space(4.0);

        // ---- Advanced
        let open = self.open.contains("adv");
        if fold(ui, "Advanced", "", open) != open {
            if open { self.open.remove("adv"); } else { self.open.insert("adv"); }
        }
        if self.open.contains("adv") {
            self.advanced_section(ui, inst, st);
        }
    }

    fn features_section(&mut self, ui: &mut egui::Ui, inst: &instance::Instance, phase: Phase) {
        let dev = inst.dir.clone();
        let prof = features::load(&dev);
        let cur = features::preset_of(&prof);
        let presets = [("full", "Full"), ("lean", "Lean"), ("minimal", "Minimal")];
        let mut changed = false;
        ui.add_space(4.0);
        ui.horizontal(|ui| {
            ui.add_space(8.0);
            let sel = presets.iter().position(|p| cur.as_deref() == Some(p.0));
            if let Some(i) = segmented(ui, &presets.map(|p| p.1), sel, true) {
                let _ = features::apply_preset(&dev, presets[i].0);
                changed = true;
            }
            ui.label(muted("Turn off what you don't use to save memory.").size(12.0));
        });
        ui.add_space(6.0);
        let mut saving = 0u64;
        for f in features::list(&dev) {
            let mut on = f.enabled;
            let what = f.desc.split_once(" Off:").map(|(a, _)| a.trim().to_string()).unwrap_or(f.desc.clone());
            ui.horizontal(|ui| {
                ui.add_space(8.0);
                ui.vertical(|ui| {
                    ui.set_width(ui.available_width() - 120.0);
                    ui.spacing_mut().item_spacing.y = 1.0;
                    ui.label(egui::RichText::new(&f.title).size(13.5).strong());
                    ui.add(egui::Label::new(muted(what).size(11.5)).wrap());
                });
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if toggle(ui, &mut on).changed() {
                        let _ = features::set(&dev, &f.id, on);
                        changed = true;
                    }
                    ui.label(muted(if f.cost_mb > 0 { format!("{} MB", f.cost_mb) } else { String::new() }).size(11.5));
                });
            }).response.on_hover_text(&f.tech);
            if !on && f.default {
                saving += f.cost_mb;
            }
            ui.add_space(4.0);
        }
        if changed {
            self.open.insert("features-changed");
        }
        ui.horizontal(|ui| {
            ui.add_space(8.0);
            if saving > 0 {
                pill(ui, &format!("Saving {saving} MB"), GREEN);
            }
            if self.open.contains("features-changed") && phase != Phase::Off {
                ui.label(egui::RichText::new("Restart to apply").color(ORANGE).size(12.5));
                if ui.add(small("⟳  Restart now")).clicked() {
                    self.open.remove("features-changed");
                    self.restart(&inst.id);
                }
            }
        });
        ui.add_space(6.0);
    }

    fn logs_section(&mut self, ui: &mut egui::Ui, inst: &instance::Instance, st: Option<&instance::Status>) {
        if self.logs.as_ref().map(|l| l.0.as_str()) != Some(&inst.id) {
            let (dirs, files) = instance::log_sources(inst, st);
            self.logs = Some((inst.id.clone(), logs::Tail::new(dirs, files)));
            self.last_log_poll = Instant::now() - Duration::from_secs(5);
        }
        if !self.log_paused && self.last_log_poll.elapsed() > Duration::from_millis(1000) {
            self.last_log_poll = Instant::now();
            if let Some((_, t)) = &mut self.logs {
                t.poll();
            }
            ui.ctx().request_repaint_after(Duration::from_millis(1000));
        }
        ui.add_space(4.0);
        ui.horizontal(|ui| {
            ui.add_space(8.0);
            ui.add(egui::TextEdit::singleline(&mut self.log_filter).hint_text("🔍  Filter").desired_width(150.0));
            let sources = self.logs.as_ref().map(|l| l.1.sources.clone()).unwrap_or_default();
            egui::ComboBox::from_id_salt("logsrc").width(130.0).selected_text(if self.log_source.is_empty() { "All sources".to_string() } else { truncate(&self.log_source, 18) }).show_ui(ui, |ui| {
                ui.selectable_value(&mut self.log_source, String::new(), "All sources");
                for s in sources {
                    ui.selectable_value(&mut self.log_source, s.clone(), s);
                }
            });
            let t = if self.log_paused { "▶" } else { "⏸" };
            if ui.add(small(t)).on_hover_text(if self.log_paused { "Follow" } else { "Pause" }).clicked() {
                self.log_paused = !self.log_paused;
            }
            if ui.add(small("Clear")).clicked() {
                if let Some((_, t)) = &mut self.logs {
                    t.lines.clear();
                }
            }
        });
        ui.add_space(4.0);
        let f = self.log_filter.to_lowercase();
        let src = self.log_source.clone();
        let paused = self.log_paused;
        if let Some((_, t)) = &self.logs {
            let rows: Vec<&logs::Line> = t.lines.iter().filter(|l| (src.is_empty() || l.source == src) && (f.is_empty() || l.text.to_lowercase().contains(&f) || l.source.to_lowercase().contains(&f))).collect();
            egui::Frame::new().fill(egui::Color32::from_rgb(0x0b, 0x0d, 0x11)).corner_radius(8).inner_margin(8).show(ui, |ui| {
                ui.set_width(ui.available_width());
                let h = egui::TextStyle::Monospace.resolve(ui.style()).size + 4.0;
                egui::ScrollArea::vertical().id_salt(("logscroll", &inst.id)).max_height(380.0).min_scrolled_height(380.0).stick_to_bottom(!paused).auto_shrink([false, false]).show_rows(ui, h, rows.len(), |ui, range| {
                    for l in &rows[range] {
                        ui.horizontal(|ui| {
                            ui.spacing_mut().item_spacing.x = 6.0;
                            ui.label(egui::RichText::new(truncate(&l.source, 16)).monospace().color(ACCENT.gamma_multiply(0.8)).size(11.5));
                            ui.add(egui::Label::new(egui::RichText::new(&l.text).monospace().size(11.5)).truncate());
                        });
                    }
                    if rows.is_empty() {
                        ui.label(muted("Nothing yet").size(12.0));
                    }
                });
            });
        }
        ui.add_space(6.0);
    }

    fn perf_section(&mut self, ui: &mut egui::Ui, st: Option<&instance::Status>) {
        ui.add_space(4.0);
        let Some(s) = st.filter(|s| s.phase != Phase::Off) else {
            ui.horizontal(|ui| {
                ui.add_space(8.0);
                ui.label(muted("Start the phone to see what it uses.").size(12.5));
            });
            ui.add_space(6.0);
            return;
        };
        let top = s.procs.iter().take(25);
        let max = s.procs.first().map(|p| p.pss_mb).unwrap_or(1.0).max(1.0);
        egui::Frame::new().fill(CARD).corner_radius(8).inner_margin(8).show(ui, |ui| {
            ui.set_width(ui.available_width());
            egui::Grid::new("perf").num_columns(3).spacing([10.0, 4.0]).min_col_width(40.0).show(ui, |ui| {
                ui.label(muted("PROCESS").size(10.5));
                ui.label(muted("MEMORY").size(10.5));
                ui.label(muted("CPU").size(10.5));
                ui.end_row();
                for p in top {
                    ui.horizontal(|ui| {
                        ui.set_width(150.0);
                        ui.add(egui::Label::new(egui::RichText::new(&p.comm).size(12.0)).truncate()).on_hover_text(format!("pid {}", p.pid));
                    });
                    ui.horizontal(|ui| {
                        let (r, pt) = ui.allocate_painter(egui::vec2(80.0, 8.0), egui::Sense::hover());
                        pt.rect_filled(r.rect, 3.0, CARD2);
                        let w = (p.pss_mb / max) as f32 * r.rect.width();
                        pt.rect_filled(egui::Rect::from_min_size(r.rect.min, egui::vec2(w, 8.0)), 3.0, ACCENT);
                        ui.label(egui::RichText::new(format!("{:.0} MB", p.pss_mb)).size(12.0));
                    });
                    ui.label(egui::RichText::new(format!("{:.0}%", p.cpu_pct)).size(12.0).color(if p.cpu_pct > 50.0 { ORANGE } else { TEXT }));
                    ui.end_row();
                }
            });
            if s.procs.len() > 25 {
                ui.label(muted(format!("and {} smaller ones", s.procs.len() - 25)).size(11.5));
            }
        });
        ui.add_space(6.0);
    }

    fn advanced_section(&mut self, ui: &mut egui::Ui, inst: &instance::Instance, st: Option<&instance::Status>) {
        ui.add_space(4.0);
        let Some((_, s)) = &mut self.settings else { return };
        let mut save = false;
        egui::Grid::new("adv").num_columns(2).spacing([12.0, 8.0]).show(ui, |ui| {
            ui.label(muted("Graphics").size(12.5)).on_hover_text("X86CODE_METAL: GPU for everything (all), only the system (backboardd), or software rendering (0). Applies at the next start.");
            let opts = [("all", "GPU"), ("backboardd", "System only"), ("0", "Software")];
            let sel = opts.iter().position(|o| o.0 == s.metal);
            if let Some(i) = segmented(ui, &opts.map(|o| o.1), sel, true) {
                s.metal = opts[i].0.into();
                save = true;
            }
            ui.end_row();
            ui.label(muted("Services").size(12.5)).on_hover_text("A64_JOBS: which background services boot. Standard is right for daily use; Full adds widgets; Slim and Minimal are for tests.");
            let opts = [("std", "Standard"), ("full", "Full"), ("slim", "Slim"), ("min", "Minimal")];
            let sel = opts.iter().position(|o| o.0 == s.jobs);
            if let Some(i) = segmented(ui, &opts.map(|o| o.1), sel, true) {
                s.jobs = opts[i].0.into();
                save = true;
            }
            ui.end_row();
            ui.label(muted("Memory cap").size(12.5)).on_hover_text("X86SIM_MEM_MAX for this phone's processes (systemd MemoryMax)");
            if ui.add(egui::TextEdit::singleline(&mut s.mem_max).desired_width(80.0)).lost_focus() {
                save = true;
            }
            ui.end_row();
            ui.label(muted("Share GPU helper").size(12.5)).on_hover_text("A64_MTLVK_SOCKET: use one mtlvk for all running phones instead of starting another");
            if toggle(ui, &mut s.share_mtlvk).changed() {
                save = true;
            }
            ui.end_row();
            ui.label(muted("Extra environment").size(12.5)).on_hover_text("A64_ENV: K=V pairs for launchd_sim, e.g. X86CODE_A64_TRACE=1");
            if ui.add(egui::TextEdit::singleline(&mut s.extra_env).hint_text("KEY=value …").desired_width(ui.available_width() - 10.0)).lost_focus() {
                save = true;
            }
            ui.end_row();
        });
        if save {
            let _ = instance::save_settings(&inst.dir, s);
            self.say("Saved. Applies at the next start.");
        }
        ui.add_space(8.0);
        ui.label(muted("Paths").size(12.0));
        let mono = |ui: &mut egui::Ui, k: &str, v: String| {
            ui.label(muted(k).size(11.5));
            ui.add(egui::Label::new(egui::RichText::new(v).monospace().size(11.0)).truncate().selectable(true));
            ui.end_row();
        };
        egui::Grid::new("paths").num_columns(2).spacing([10.0, 3.0]).show(ui, |ui| {
            mono(ui, "Device", inst.dir.display().to_string());
            mono(ui, "State", st.map(|s| s.state_dir.clone()).unwrap_or(inst.state_dir.clone()).display().to_string());
            mono(ui, "Prefix", st.map(|s| s.prefix.clone()).unwrap_or(inst.prefix.clone()).display().to_string());
            mono(ui, "Frame ring", inst.ring.display().to_string());
            mono(ui, "Input socket", inst.sock.display().to_string());
            mono(ui, "Runtime", inst.runtime_root.display().to_string());
            mono(ui, "Cache", x86code_core::sim::jc_for(&inst.runtime_root).display().to_string());
            mono(ui, "UDID", inst.udid.clone());
            mono(ui, "Model id", inst.model_id.clone());
        });
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            if ui.add(small("Open in its own window")).on_hover_text("The standalone viewer (simview) follows the phone booted with the default dirs").clicked() {
                match x86code_core::sim::show() {
                    Ok(()) => self.say("Window opened"),
                    Err(e) => self.say(format!("{e:#}")),
                }
            }
            if ui.add(small("Pre-translate for this iOS")).clicked() {
                self.aot.select(inst.runtime_root.clone(), Some(inst.dir.clone()));
                self.view = View::Aot;
            }
        });
        ui.add_space(6.0);
    }
}
