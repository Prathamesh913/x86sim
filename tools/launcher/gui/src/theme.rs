//! Look: palette and the small widgets (cards, pills, switches, segmented controls, fold headers).
use eframe::egui;

pub const BG: egui::Color32 = egui::Color32::from_rgb(0x0e, 0x10, 0x14);
pub const RAIL: egui::Color32 = egui::Color32::from_rgb(0x13, 0x16, 0x1c);
pub const CARD: egui::Color32 = egui::Color32::from_rgb(0x19, 0x1d, 0x25);
pub const CARD2: egui::Color32 = egui::Color32::from_rgb(0x21, 0x26, 0x30);
pub const BORDER: egui::Color32 = egui::Color32::from_rgb(0x2a, 0x30, 0x3c);
pub const TEXT: egui::Color32 = egui::Color32::from_rgb(0xe8, 0xeb, 0xf1);
pub const MUTED: egui::Color32 = egui::Color32::from_rgb(0x8c, 0x94, 0xa6);
pub const ACCENT: egui::Color32 = egui::Color32::from_rgb(0x4c, 0x8d, 0xff);
pub const GREEN: egui::Color32 = egui::Color32::from_rgb(0x3d, 0xd6, 0x8c);
pub const ORANGE: egui::Color32 = egui::Color32::from_rgb(0xff, 0xb0, 0x4c);
pub const RED: egui::Color32 = egui::Color32::from_rgb(0xff, 0x6b, 0x6b);
pub const BEZEL: egui::Color32 = egui::Color32::from_rgb(0x05, 0x06, 0x08);

pub fn theme(ctx: &egui::Context) {
    use egui::{FontFamily, FontId, TextStyle};
    ctx.set_theme(egui::Theme::Dark);
    ctx.style_mut(|s| {
        s.text_styles = [
            (TextStyle::Heading, FontId::new(24.0, FontFamily::Proportional)),
            (TextStyle::Body, FontId::new(14.0, FontFamily::Proportional)),
            (TextStyle::Button, FontId::new(14.0, FontFamily::Proportional)),
            (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
            (TextStyle::Monospace, FontId::new(12.5, FontFamily::Monospace)),
        ]
        .into();
        s.spacing.item_spacing = egui::vec2(10.0, 8.0);
        s.spacing.button_padding = egui::vec2(12.0, 6.0);
        s.spacing.interact_size.y = 30.0;
        s.spacing.slider_width = 160.0;
        let v = &mut s.visuals;
        v.panel_fill = BG;
        v.window_fill = CARD;
        v.extreme_bg_color = CARD2;
        v.faint_bg_color = egui::Color32::from_rgb(0x1d, 0x22, 0x2b);
        v.override_text_color = Some(TEXT);
        v.hyperlink_color = ACCENT;
        v.selection.bg_fill = ACCENT.gamma_multiply(0.35);
        v.selection.stroke = egui::Stroke::new(1.0_f32, ACCENT);
        v.window_corner_radius = 12.into();
        v.menu_corner_radius = 10.into();
        v.window_stroke = egui::Stroke::new(1.0_f32, BORDER);
        v.slider_trailing_fill = true;
        let r: egui::CornerRadius = 8.into();
        for w in [&mut v.widgets.noninteractive, &mut v.widgets.inactive, &mut v.widgets.hovered, &mut v.widgets.active, &mut v.widgets.open] {
            w.corner_radius = r;
        }
        v.widgets.noninteractive.bg_stroke = egui::Stroke::new(1.0_f32, BORDER);
        v.widgets.noninteractive.fg_stroke = egui::Stroke::new(1.0_f32, TEXT);
        v.widgets.inactive.bg_fill = CARD2;
        v.widgets.inactive.weak_bg_fill = CARD2;
        v.widgets.inactive.bg_stroke = egui::Stroke::new(1.0_f32, BORDER);
        v.widgets.inactive.fg_stroke = egui::Stroke::new(1.0_f32, TEXT);
        v.widgets.hovered.bg_fill = egui::Color32::from_rgb(0x2a, 0x31, 0x3d);
        v.widgets.hovered.weak_bg_fill = egui::Color32::from_rgb(0x2a, 0x31, 0x3d);
        v.widgets.hovered.bg_stroke = egui::Stroke::new(1.0_f32, ACCENT.gamma_multiply(0.6));
        v.widgets.hovered.expansion = 0.0;
        v.widgets.active.bg_fill = egui::Color32::from_rgb(0x33, 0x3b, 0x49);
        v.widgets.active.weak_bg_fill = egui::Color32::from_rgb(0x33, 0x3b, 0x49);
        v.widgets.active.bg_stroke = egui::Stroke::new(1.0_f32, ACCENT);
        v.widgets.active.expansion = 0.0;
    });
}

pub fn card<R>(ui: &mut egui::Ui, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    egui::Frame::new().fill(CARD).stroke(egui::Stroke::new(1.0_f32, BORDER)).corner_radius(12).inner_margin(16).show(ui, |ui| {
        ui.set_width(ui.available_width());
        add(ui)
    }).inner
}

pub fn muted(t: impl Into<String>) -> egui::RichText {
    egui::RichText::new(t).color(MUTED)
}

pub fn page_header(ui: &mut egui::Ui, title: &str, sub: &str) {
    ui.add_space(4.0);
    ui.label(egui::RichText::new(title).size(24.0).strong());
    if !sub.is_empty() {
        ui.label(muted(sub));
    }
    ui.add_space(10.0);
}

pub fn primary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(egui::Color32::WHITE).strong()).fill(ACCENT).stroke(egui::Stroke::NONE).min_size(egui::vec2(100.0, 34.0))
}

pub fn danger(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(egui::Color32::WHITE).strong()).fill(RED.gamma_multiply(0.75)).stroke(egui::Stroke::NONE).min_size(egui::vec2(100.0, 34.0))
}

pub fn secondary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(text.into()).min_size(egui::vec2(84.0, 34.0))
}

pub fn small(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).size(12.5)).min_size(egui::vec2(0.0, 26.0))
}

pub fn pill(ui: &mut egui::Ui, text: &str, col: egui::Color32) {
    egui::Frame::new().fill(col.gamma_multiply(0.16)).corner_radius(20).inner_margin(egui::Margin::symmetric(9, 3)).show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 6.0;
            let (r, p) = ui.allocate_painter(egui::vec2(8.0, 8.0), egui::Sense::hover());
            p.circle_filled(r.rect.center(), 4.0, col);
            ui.label(egui::RichText::new(text).color(col).size(12.0).strong());
        });
    });
}

pub fn stat(ui: &mut egui::Ui, w: f32, label: &str, value: String, col: egui::Color32) {
    egui::Frame::new().fill(CARD2).corner_radius(10).inner_margin(12).show(ui, |ui| {
        ui.set_width(w - 24.0);
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 2.0;
            ui.label(muted(label).size(11.0));
            ui.label(egui::RichText::new(value).size(18.0).strong().color(col));
        });
    });
}

pub fn stat_row(ui: &mut egui::Ui, items: Vec<(&str, String, egui::Color32)>) {
    let gap = 10.0;
    let w = (ui.available_width() - gap * (items.len() as f32 - 1.0)) / items.len() as f32;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = gap;
        for (l, v, c) in items {
            stat(ui, w, l, v, c);
        }
    });
}

/// iOS-style switch
pub fn toggle(ui: &mut egui::Ui, on: &mut bool) -> egui::Response {
    let size = egui::vec2(40.0, 22.0);
    let (rect, mut resp) = ui.allocate_exact_size(size, egui::Sense::click());
    if resp.clicked() {
        *on = !*on;
        resp.mark_changed();
    }
    let t = ui.ctx().animate_bool_responsive(resp.id, *on);
    let bg = egui::Color32::from_rgb(0x3a, 0x41, 0x4f).lerp_to_gamma(GREEN, t);
    let p = ui.painter();
    p.rect_filled(rect, 11.0, bg);
    let x = egui::lerp((rect.left() + 11.0)..=(rect.right() - 11.0), t);
    p.circle_filled(egui::pos2(x, rect.center().y), 8.5, egui::Color32::WHITE);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// segmented control; returns the clicked index
pub fn segmented(ui: &mut egui::Ui, items: &[&str], sel: Option<usize>, enabled: bool) -> Option<usize> {
    let mut out = None;
    egui::Frame::new().fill(CARD2).corner_radius(9).inner_margin(3).show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 2.0;
            for (i, l) in items.iter().enumerate() {
                let on = sel == Some(i);
                let b = egui::Button::new(egui::RichText::new(*l).size(13.0).color(if on { egui::Color32::WHITE } else { MUTED }))
                    .fill(if on { ACCENT } else { egui::Color32::TRANSPARENT })
                    .stroke(egui::Stroke::NONE)
                    .corner_radius(7)
                    .min_size(egui::vec2(0.0, 26.0));
                if ui.add_enabled(enabled, b).clicked() {
                    out = Some(i);
                }
            }
        });
    });
    out
}

pub fn nav_item(ui: &mut egui::Ui, icon: &str, label: &str, on: bool, dot: Option<egui::Color32>) -> bool {
    let w = ui.available_width();
    let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, 36.0), egui::Sense::click());
    let p = ui.painter();
    if on {
        p.rect_filled(rect, 9.0, ACCENT.gamma_multiply(0.18));
        p.rect_filled(egui::Rect::from_min_size(rect.left_top() + egui::vec2(0.0, 9.0), egui::vec2(3.0, 18.0)), 2.0, ACCENT);
    } else if resp.hovered() {
        p.rect_filled(rect, 9.0, CARD2);
    }
    let col = if on { TEXT } else { MUTED };
    p.text(rect.left_center() + egui::vec2(14.0, 0.0), egui::Align2::LEFT_CENTER, icon, egui::FontId::proportional(15.0), if on { ACCENT } else { MUTED });
    let label_rect = egui::Rect::from_min_max(rect.left_top() + egui::vec2(40.0, 0.0), rect.right_bottom() - egui::vec2(24.0, 0.0));
    let galley = ui.fonts_mut(|f| f.layout(label.to_string(), egui::FontId::proportional(14.0), col, label_rect.width()));
    let galley = if galley.rows.len() > 1 { ui.fonts_mut(|f| f.layout_no_wrap(truncate(label, 22), egui::FontId::proportional(14.0), col)) } else { galley };
    p.galley(egui::pos2(label_rect.left(), rect.center().y - galley.size().y / 2.0), galley, col);
    if let Some(d) = dot {
        p.circle_filled(rect.right_center() - egui::vec2(14.0, 0.0), 4.0, d);
    }
    resp.on_hover_cursor(egui::CursorIcon::PointingHand).clicked()
}

pub fn truncate(s: &str, n: usize) -> String {
    if s.chars().count() <= n { s.to_string() } else { format!("{}…", s.chars().take(n.saturating_sub(1)).collect::<String>()) }
}

/// Fold-out section header: chevron, title, summary at the right. Returns the new open state.
pub fn fold(ui: &mut egui::Ui, title: &str, summary: &str, open: bool) -> bool {
    let w = ui.available_width();
    let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, 36.0), egui::Sense::click());
    let p = ui.painter();
    if resp.hovered() {
        p.rect_filled(rect, 8.0, CARD2);
    }
    let t = ui.ctx().animate_bool_responsive(resp.id.with("chev"), open);
    let c = rect.left_center() + egui::vec2(14.0, 0.0);
    let ang = t * std::f32::consts::FRAC_PI_2;
    let rot = |v: egui::Vec2| egui::vec2(v.x * ang.cos() - v.y * ang.sin(), v.x * ang.sin() + v.y * ang.cos());
    let pts = [c + rot(egui::vec2(-2.0, -5.0)), c + rot(egui::vec2(3.0, 0.0)), c + rot(egui::vec2(-2.0, 5.0))];
    p.add(egui::Shape::line(pts.to_vec(), egui::Stroke::new(1.8_f32, if open { ACCENT } else { MUTED })));
    p.text(rect.left_center() + egui::vec2(30.0, 0.0), egui::Align2::LEFT_CENTER, title, egui::FontId::proportional(14.5), TEXT);
    if !summary.is_empty() {
        p.text(rect.right_center() - egui::vec2(8.0, 0.0), egui::Align2::RIGHT_CENTER, summary, egui::FontId::proportional(12.0), MUTED);
    }
    if resp.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() { !open } else { open }
}

pub fn hline(ui: &mut egui::Ui) {
    let r = ui.available_rect_before_wrap();
    ui.painter().hline(r.x_range(), r.top(), egui::Stroke::new(1.0_f32, BORDER));
    ui.add_space(1.0);
}

pub fn fmt_s(s: f64) -> String {
    if s >= 3600.0 {
        format!("{}h {:02}m", (s / 3600.0) as u64, (s as u64 % 3600) / 60)
    } else if s >= 60.0 {
        format!("{}m {:02}s", (s / 60.0) as u64, s as u64 % 60)
    } else {
        format!("{s:.0}s")
    }
}

pub fn fmt_mb(mb: f64) -> String {
    if mb >= 1000.0 { format!("{:.1} GB", mb / 1024.0) } else { format!("{mb:.0} MB") }
}
