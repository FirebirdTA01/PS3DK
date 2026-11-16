use eframe::egui::{self, Color32, Pos2, RichText, Rounding, Sense, Vec2};
use crate::auto::PassesView;

use ps3dk_ui as ui;

/// Shown when `!by_tag().tagged`.
pub const EMPTY_NOTICE: &str = "no tags: the app calls no PS3DK_PROF_AUTO_TAG";

/// What was actually rendered this frame (asserted by tests).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Drawn {
    pub empty: bool,
    pub empty_message: String,
    pub tag_rows: usize,
    pub range_rows: usize,
    pub frame_rows: usize,
}

/// Horizontal bar sparkline: one colored bar per value, height proportional to
/// value / max(values). Uses the same painter idiom as the badges (`ui.painter()`).
fn sparkline(ui: &mut egui::Ui, values: &[u64], width: f32, height: f32, accent: Color32) {
    if values.is_empty() {
        ui.add_space(2.0);
        return;
    }
    let (rect, _resp) = ui.allocate_exact_size(Vec2::new(width, height), Sense::hover());
    if ui.is_rect_visible(rect) {
        let max = values.iter().max().copied().unwrap_or(0) as f64;
        let painter = ui.painter();
        if max == 0.0 {
            return;
        }
        let n = values.len() as f32;
        let slot = (width / n).max(1.0);
        let gap = (slot * 0.15).min(1.5);
        for (i, v) in values.iter().enumerate() {
            let x = rect.left() + i as f32 * slot + gap * 0.5;
            let frac = (*v as f64 / max).clamp(0.0, 1.0);
            let bh = (height * frac as f32).max(1.0);
            let bar = egui::Rect::from_min_size(Pos2::new(x, rect.bottom() - bh), Vec2::new((slot - gap).max(0.5), bh));
            painter.rect_filled(bar, Rounding::same(1.0), accent);
        }
    }
}

fn group_card(
    p: &mut egui::Ui,
    card_title: &str,
    subtitle: Option<&str>,
    headers: &[&str],
    rows: &[(String, Vec<String>, Vec<u64>)],
    accent: Color32,
    out_rows: &mut usize,
) {
    ui::card(p, |c| {
        ui::card_header(c, card_title, subtitle, |_| {});
        c.add_space(ui::SPACE_XS);
        if rows.is_empty() {
            ui::field_message(c, ui::Tone::Neutral, "no rows recorded for this capture");
            return;
        }
        *out_rows = rows.len();
        let ncols = headers.len() + 1;
        egui::ScrollArea::horizontal()
            .id_salt(format!("passes_scroll_{card_title:?}"))
            .show(c, |c2| {
                egui::Grid::new(format!("passes_grid_{card_title:?}"))
                    .num_columns(ncols)
                    .striped(true)
                    .spacing([ui::SPACE_MD, ui::SPACE_XS])
                    .show(c2, |g| {
                        for h in headers {
                            g.label(RichText::new(*h).strong());
                        }
                        g.label(RichText::new("per-frame").strong());
                        g.end_row();
                        for (label, cols, values) in rows {
                            g.label(RichText::new(label.clone()).strong());
                            for col in cols {
                                g.label(col.clone());
                            }
                            g.allocate_ui_with_layout(
                                Vec2::new(100.0, 18.0),
                                egui::Layout::top_down(egui::Align::Center),
                                |u| sparkline(u, values, 100.0, 18.0, accent),
                            );
                            g.end_row();
                        }
                    });
            });
    });
    p.add_space(ui::SPACE_SM);
}

pub fn page(p: &mut egui::Ui, v: &PassesView, drawn: &mut Drawn) {
    ui::page_header(p, "Passes", "RSX time attributed by tag and named range");

    if !v.has_tags() {
        drawn.empty = true;
        drawn.empty_message = EMPTY_NOTICE.to_string();
        ui::empty_state(p, EMPTY_NOTICE, "The capture's writer does not record auto tags (the app calls no PS3DK_PROF_AUTO_TAG).");
        return;
    }

    let pal = ui::Palette::of(p);

    // --- Per-tag table ---
    let mut tag_rows: Vec<(String, Vec<String>, Vec<u64>)> = Vec::new();
    for (tag, (frames_n, ops, busy_sum, med, p95)) in &v.tags {
        let per_frame: Vec<u64> = v
            .by
            .frames
            .iter()
            .filter_map(|f| f.tags.get(tag).map(|r| r.busy_raw))
            .collect();
        tag_rows.push((
            format!("{tag:#x}"),
            vec![
                frames_n.to_string(),
                format!("{ops}"),
                busy_sum.to_string(),
                med.to_string(),
                p95.to_string(),
            ],
            per_frame,
        ));
    }
    group_card(
        p,
        "PER TAG",
        Some("Frames, ops, total busy, and per-frame median / p95 busy ticks"),
        &["tag", "frames", "ops", "busy sum", "median", "p95"],
        &tag_rows,
        pal.accent,
        &mut drawn.tag_rows,
    );

    // --- Per-range table ---
    let mut range_rows: Vec<(String, Vec<String>, Vec<u64>)> = Vec::new();
    for (name, (frames_n, count, raw_sum, med, p95)) in &v.ranges {
        let per_frame: Vec<u64> = v
            .by
            .frames
            .iter()
            .filter_map(|f| f.ranges.get(name).map(|r| r.raw))
            .collect();
        range_rows.push((
            name.clone(),
            vec![
                frames_n.to_string(),
                count.to_string(),
                raw_sum.to_string(),
                med.to_string(),
                p95.to_string(),
            ],
            per_frame,
        ));
    }
    group_card(
        p,
        "PER RANGE",
        Some("Frames, range count, total raw ticks, and per-frame median / p95 raw ticks"),
        &["range", "frames", "ranges", "sum", "median", "p95"],
        &range_rows,
        pal.success,
        &mut drawn.range_rows,
    );

    // --- Per-frame untimed / unrepresented ---
    // Only frames with something to show get a row: a capture has thousands of
    // frames, and a grid row per frame would be painted on every repaint.
    let quiet = v.by.frames.iter().filter(|f| f.untimed == 0 && f.unrepresented == 0).count();
    let mut frows: Vec<(String, Vec<String>, Vec<u64>)> = Vec::new();
    for f in v.by.frames.iter().filter(|f| f.untimed != 0 || f.unrepresented != 0) {
        frows.push((
            format!("#{}", f.frame),
            vec![f.untimed.to_string(), u64::from(f.unrepresented).to_string()],
            vec![f.untimed, u64::from(f.unrepresented)],
        ));
    }
    group_card(
        p,
        "PER FRAME (untimed / unrepresented)",
        Some(&format!("Frames with draws covered but not recorded as a timed row; {quiet} other frame(s) have none")),
        &["frame", "untimed", "unrepresented"],
        &frows,
        pal.warning,
        &mut drawn.frame_rows,
    );
}
