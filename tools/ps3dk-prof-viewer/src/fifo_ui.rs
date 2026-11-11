//! The FIFO wrap waits page: one lane per phase on the PPU timebase axis, open ends marked, UNKNOWN phases listed.
use crate::fifo::{FifoView, CAPTION, NOT_ENABLED, PAINT_BARS, RSX_STATUS, TITLE};
use eframe::egui::{self, Align2, Color32, FontId, Pos2, Rect, Sense, Stroke, Vec2};
use ps3dk_ui as ui;

#[derive(Debug, Default)]
pub struct Drawn {
    pub status: String,
    pub bars: usize,
    pub open_ends: usize,
    pub unknown_rows: usize,
    pub rows: usize,
    pub lane_rect: Option<Rect>,
    pub hovered: Option<String>,
}

pub fn page(p: &mut egui::Ui, v: &FifoView, d: &mut Drawn) {
    *d = Drawn::default();
    ui::page_header(p, TITLE, CAPTION);
    d.status = v.status();
    ui::field_message(p, ui::Tone::Neutral, &d.status);
    p.label(RSX_STATUS);
    if !v.capability {
        return;
    }
    if !v.config.is_empty() {
        p.label(&v.config);
    }
    p.label(&v.counts);
    if v.refusal.is_some() {
        return;
    }
    p.label(NOT_ENABLED);
    ui::section_label(p, "PHASE BARS (PPU timebase)");
    let h = 64.0;
    let (rect, _) = p.allocate_exact_size(Vec2::new(p.available_width().max(200.0), h), Sense::hover());
    d.lane_rect = Some(rect);
    let painter = p.painter_at(rect);
    painter.rect_stroke(rect, 0.0, Stroke::new(1.0_f32, Color32::GRAY));
    let label_w = 70.0;
    for (i, name) in ["phase 1", "phase 2"].iter().enumerate() {
        painter.text(Pos2::new(rect.left() + 4.0, rect.top() + 16.0 + 32.0 * i as f32), Align2::LEFT_CENTER, name,
            FontId::proportional(12.0), Color32::GRAY);
    }
    if let Some((lo, hi)) = v.axis {
        let span = (hi.saturating_sub(lo)).max(1) as f64;
        let x0 = rect.left() + label_w;
        let w = (rect.width() - label_w - 8.0).max(1.0) as f64;
        let x = |t: u64| x0 + (((t.saturating_sub(lo)) as f64 / span) * w) as f32;
        let hover = p.ctx().pointer_hover_pos();
        for b in v.bars.iter().take(PAINT_BARS) {
            let top = rect.top() + 6.0 + 32.0 * f32::from(b.phase - 1);
            let end = b.end_tb.unwrap_or(hi);
            let r = Rect::from_min_max(Pos2::new(x(b.start_tb), top), Pos2::new(x(end).max(x(b.start_tb) + 2.0), top + 20.0));
            let color = if b.phase == 1 { Color32::from_rgb(70, 130, 200) } else { Color32::from_rgb(200, 140, 60) };
            painter.rect_filled(r, 2.0, color);
            if b.is_open() {
                // the open end: a dashed edge and a label, never a closed bar
                painter.text(Pos2::new(r.right() - 2.0, r.center().y), Align2::RIGHT_CENTER, "OPEN ▶",
                    FontId::proportional(11.0), Color32::WHITE);
                d.open_ends += 1;
            }
            if hover.is_some_and(|h| r.contains(h)) {
                d.hovered = Some(v.tooltip(b));
            }
            d.bars += 1;
        }
    } else {
        painter.text(rect.center(), Align2::CENTER_CENTER, "No phase bar: no complete or open phase was recorded.",
            FontId::proportional(12.0), Color32::GRAY);
    }
    if let Some(t) = &d.hovered {
        p.label(t);
    }
    ui::section_label(p, "WRAPS");
    p.label("A missing end is OPEN or UNKNOWN, never zero. Entered and completed counts are separate.");
    egui::ScrollArea::vertical().id_salt("fifo-wraps").max_height(320.0).show_rows(p, 22.0, v.rows.len(), |p, range| {
        for i in range {
            let row = &v.rows[i];
            p.add(egui::Label::new(&row.text).truncate()).on_hover_text(&row.text);
            d.rows += 1;
        }
    });
    d.unknown_rows = v.unknown.len();
}
