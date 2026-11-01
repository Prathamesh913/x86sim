//! Home: every phone as a card with its live screen, state, memory and CPU, Start/Stop; "Add phone".
use crate::theme::*;
use crate::{viewer, AddDialog, App, View};
use eframe::egui;
use std::sync::{Arc, Mutex};
use x86code_core::instance::Phase;

impl App {
    pub fn home(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            page_header(ui, "Phones", "");
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Min), |ui| {
                ui.add_space(4.0);
                if ui.add(primary("＋  Add phone")).clicked() {
                    let rt = self.runtimes.iter().position(|r| r.present).unwrap_or(0);
                    self.add = Some(AddDialog { model: 0, runtime: rt, name: String::new(), busy: Arc::new(Mutex::new(None)), working: false });
                }
            });
        });
        if self.insts.is_empty() {
            card(ui, |ui| ui.label(muted("No phones yet. Add one to get started.")));
            return;
        }
        let cw = self.hub.card_w;
        let gap = 14.0;
        let per_row = ((ui.available_width() + gap) / (cw + gap)).floor().max(1.0) as usize;
        let ids: Vec<String> = self.insts.iter().map(|i| i.id.clone()).collect();
        for row in ids.chunks(per_row) {
            ui.horizontal_top(|ui| {
                ui.spacing_mut().item_spacing.x = gap;
                for id in row {
                    self.card(ui, id, cw);
                }
            });
            ui.add_space(gap);
        }
    }

    fn card(&mut self, ui: &mut egui::Ui, id: &str, w: f32) {
        let Some(inst) = self.inst(id).cloned() else { return };
        let st = self.status.get(id).cloned();
        let phase = st.as_ref().map(|s| s.phase).unwrap_or(Phase::Off);
        let thumbs = self.hub.thumbnails;
        let mut open = false;
        egui::Frame::new().fill(CARD).stroke(egui::Stroke::new(1.0_f32, BORDER)).corner_radius(14).inner_margin(14).show(ui, |ui| {
            ui.set_width(w - 28.0);
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 4.0;
                ui.horizontal(|ui| {
                    ui.add(egui::Label::new(egui::RichText::new(&inst.name).size(15.5).strong()).truncate());
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| match phase {
                        Phase::Running => pill(ui, "Running", GREEN),
                        Phase::Starting => pill(ui, "Starting…", ORANGE),
                        Phase::Stopping => pill(ui, "Stopping…", ORANGE),
                        Phase::Off if !inst.runtime_present => pill(ui, "Needs iOS", RED),
                        Phase::Off => pill(ui, "Off", MUTED),
                    });
                });
                ui.label(muted(format!("{} · {}", inst.model, inst.ios)).size(12.0));
                ui.add_space(6.0);
                // the screen
                let avail = egui::vec2(w - 28.0, (w - 28.0) * 1.35);
                let live = matches!(phase, Phase::Running | Phase::Starting) && thumbs;
                let placeholder = match phase {
                    Phase::Off if !inst.runtime_present => format!("{} not downloaded", inst.ios),
                    Phase::Off => "Off".into(),
                    Phase::Starting => "Starting…".into(),
                    Phase::Stopping => "Stopping…".into(),
                    Phase::Running => if thumbs { "" } else { "Running" }.into(),
                };
                let (rect, resp) = ui.allocate_exact_size(avail, egui::Sense::click());
                let mut child = ui.new_child(egui::UiBuilder::new().max_rect(rect).layout(egui::Layout::centered_and_justified(egui::Direction::TopDown)));
                if let Some(v) = self.viewer(id) {
                    child.allocate_ui_with_layout(avail, egui::Layout::top_down(egui::Align::Center), |ui| {
                        v.show(ui, avail, live, &placeholder, false, viewer::THUMB);
                    });
                }
                if resp.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() {
                    open = true;
                }
                ui.add_space(8.0);
                match &st {
                    Some(s) if phase != Phase::Off => {
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(fmt_mb(s.pss_mb)).size(13.0).strong());
                            ui.label(muted("memory").size(11.5));
                            ui.add_space(6.0);
                            ui.label(egui::RichText::new(format!("{:.0}%", s.cpu_pct)).size(13.0).strong());
                            ui.label(muted("CPU").size(11.5));
                            if let Some(u) = s.uptime_s {
                                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                    ui.label(muted(format!("up {}", fmt_s(u))).size(11.5));
                                });
                            }
                        });
                    }
                    _ => {
                        ui.label(muted(format!("{} × {}", inst.screen.0, inst.screen.1)).size(11.5));
                    }
                }
                ui.add_space(6.0);
                ui.horizontal(|ui| {
                    let bw = (ui.available_width() - 8.0) / 2.0;
                    match phase {
                        Phase::Off if !inst.runtime_present => {
                            if ui.add(primary("Download iOS").min_size(egui::vec2(bw, 32.0))).clicked() {
                                self.view = View::Ios;
                            }
                        }
                        Phase::Off => {
                            if ui.add(primary("▶  Start").min_size(egui::vec2(bw, 32.0))).clicked() {
                                self.start(id);
                            }
                        }
                        Phase::Running | Phase::Starting => {
                            if ui.add(danger("■  Stop").min_size(egui::vec2(bw, 32.0))).clicked() {
                                self.stop(id);
                            }
                        }
                        Phase::Stopping => {
                            ui.add_enabled(false, secondary("Stopping…").min_size(egui::vec2(bw, 32.0)));
                        }
                    }
                    if ui.add(secondary("Open").min_size(egui::vec2(bw, 32.0))).clicked() {
                        open = true;
                    }
                });
            });
        });
        if open {
            self.view = View::Phone(id.to_string());
        }
    }

    pub fn add_dialog(&mut self, ctx: &egui::Context) {
        let Some(mut d) = self.add.take() else { return };
        let mut close = false;
        let mut create = false;
        if let Some(r) = d.busy.lock().unwrap().take() {
            d.working = false;
            match r {
                Ok(p) => {
                    self.refresh_instances();
                    let id = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
                    self.say("Phone added");
                    self.view = View::Phone(id);
                    return;
                }
                Err(e) => self.say(format!("Couldn't add it: {e}")),
            }
        }
        egui::Modal::new("add-phone".into()).frame(egui::Frame::new().fill(CARD).stroke(egui::Stroke::new(1.0_f32, BORDER)).corner_radius(14).inner_margin(22)).show(ctx, |ui| {
            ui.set_width(420.0);
            ui.label(egui::RichText::new("Add a phone").size(20.0).strong());
            ui.label(muted("Pick a model and an iOS version. It gets its own data, settings and window."));
            ui.add_space(12.0);
            egui::Grid::new("addgrid").num_columns(2).spacing([14.0, 12.0]).show(ui, |ui| {
                ui.label(muted("Model"));
                egui::ComboBox::from_id_salt("addmodel").width(260.0).selected_text(self.models.get(d.model).map(|m| m.0.clone()).unwrap_or_default()).show_ui(ui, |ui| {
                    for (i, m) in self.models.iter().enumerate() {
                        ui.selectable_value(&mut d.model, i, &m.0);
                    }
                });
                ui.end_row();
                ui.label(muted("iOS version"));
                let label = |r: &x86code_core::host::Runtime| if r.present { format!("iOS {}", r.version) } else { format!("iOS {} (not downloaded)", r.version) };
                egui::ComboBox::from_id_salt("addrt").width(260.0).selected_text(self.runtimes.get(d.runtime).map(label).unwrap_or("none downloaded".into())).show_ui(ui, |ui| {
                    for (i, r) in self.runtimes.iter().enumerate() {
                        ui.add_enabled_ui(r.present, |ui| {
                            ui.selectable_value(&mut d.runtime, i, label(r));
                        });
                    }
                });
                ui.end_row();
                ui.label(muted("Name"));
                ui.add(egui::TextEdit::singleline(&mut d.name).hint_text("Optional, e.g. Test phone").desired_width(260.0));
                ui.end_row();
            });
            ui.add_space(16.0);
            ui.horizontal(|ui| {
                let ok = self.runtimes.get(d.runtime).is_some_and(|r| r.present) && !d.working;
                if ui.add_enabled(ok, primary("Add")).clicked() {
                    create = true;
                }
                if ui.add_enabled(!d.working, secondary("Cancel")).clicked() {
                    close = true;
                }
                if d.working {
                    ui.spinner();
                    ui.label(muted("Setting it up…"));
                }
            });
        });
        if create {
            if let (Some(m), Some(r)) = (self.models.get(d.model).cloned(), self.runtimes.get(d.runtime).cloned()) {
                d.working = true;
                let busy = d.busy.clone();
                let name = d.name.clone();
                let ctx = ctx.clone();
                std::thread::spawn(move || {
                    let r = x86code_core::sim::create_device(&m.0, &name, &r.root).map_err(|e| format!("{e:#}"));
                    *busy.lock().unwrap() = Some(r);
                    ctx.request_repaint();
                });
            }
        }
        if !close {
            self.add = Some(d);
        }
    }
}
