//! The Passes page: RSX time per AUTO tag and per named range, from the reader's by-tag inventory.

use eframe::egui::{self, Color32, Pos2, RichText, Rounding, Sense, Vec2};
use crate::auto::PassesView;

use ps3dk_ui as ui;

/// Shown when the capture's AUTO writer has no tag capability (CONFIG without 1023/11).
pub const EMPTY_NOTICE: &str = "No tag or range data in this capture";
/// The empty state's explanation: a capability statement about the capture, not a claim about the app.
pub const EMPTY_DETAIL: &str = "This capture's automatic-profiler writer does not record tags or named ranges \
(its CONFIG lacks the tag capability), so per-tag and per-range RSX time is not available. Captures from beta 4.9 \
and later record them when the application uses PS3DK_PROF_AUTO_TAG or the AUTO range macros.";

/// Width and height of a sparkline cell, in points.
pub const SPARK_W: f32 = 100.0;
pub const SPARK_H: f32 = 18.0;

/// What was actually rendered this frame (asserted by tests).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Drawn {
    pub empty: bool,
    pub empty_message: String,
    pub tag_rows: usize,
    pub range_rows: usize,
    pub frame_rows: usize,
}

/// Reduces a series to at most `cols` columns, each the maximum of the values that fall into it, so a long series
/// fits its cell and no spike is lost. A zero column stays zero. Fewer values than columns are returned unchanged.
pub fn sparkline_columns(values: &[u64], cols: usize) -> Vec<u64> {
    if cols == 0 || values.len() <= cols {
        return values.to_vec();
    }
    let n = values.len();
    (0..cols)
        .map(|c| {
            let start = c * n / cols;
            let end = ((c + 1) * n / cols).max(start + 1).min(n);
            values[start..end].iter().copied().max().unwrap_or(0)
        })
        .collect()
}

/// One bar per column, height proportional to value / max, painted clipped to the allocated rect. A zero value
/// paints nothing.
fn sparkline(ui: &mut egui::Ui, values: &[u64], accent: Color32) {
    let (rect, _resp) = ui.allocate_exact_size(Vec2::new(SPARK_W, SPARK_H), Sense::hover());
    if values.is_empty() || !ui.is_rect_visible(rect) {
        return;
    }
    let cols = sparkline_columns(values, rect.width().floor().max(1.0) as usize);
    let max = cols.iter().copied().max().unwrap_or(0);
    if max == 0 {
        return;
    }
    let painter = ui.painter_at(rect);
    let slot = rect.width() / cols.len() as f32;
    let gap = if slot >= 3.0 { (slot * 0.15).min(1.5) } else { 0.0 };
    for (i, &v) in cols.iter().enumerate() {
        if v == 0 {
            continue;
        }
        let bh = (SPARK_H * (v as f64 / max as f64) as f32).max(1.0);
        let x = rect.left() + i as f32 * slot + gap * 0.5;
        let bar = egui::Rect::from_min_size(Pos2::new(x, rect.bottom() - bh), Vec2::new((slot - gap).max(0.5), bh));
        painter.rect_filled(bar, Rounding::same(1.0), accent);
    }
}

/// A table cell: shown text plus optional hover text.
type Cell = (String, Option<String>);

fn group_card(
    p: &mut egui::Ui,
    card_title: &str,
    subtitle: &str,
    headers: &[&str],
    rows: &[(String, Vec<Cell>, Vec<u64>)],
    accent: Color32,
    out_rows: &mut usize,
) {
    ui::card(p, |c| {
        ui::card_header(c, card_title, Some(subtitle), |_| {});
        c.add_space(ui::SPACE_XS);
        if rows.is_empty() {
            ui::field_message(c, ui::Tone::Neutral, "No rows in this capture.");
            return;
        }
        *out_rows = rows.len();
        egui::ScrollArea::horizontal().id_salt(format!("passes_scroll_{card_title}")).show(c, |c2| {
            egui::Grid::new(format!("passes_grid_{card_title}"))
                .num_columns(headers.len() + 1)
                .striped(true)
                .spacing([ui::SPACE_MD, ui::SPACE_XS])
                .show(c2, |g| {
                    for h in headers {
                        g.label(RichText::new(*h).strong());
                    }
                    g.label(RichText::new("per frame").strong());
                    g.end_row();
                    for (label, cols, values) in rows {
                        g.label(RichText::new(label.as_str()).strong());
                        for (text, hover) in cols {
                            let r = g.label(text.as_str());
                            if let Some(h) = hover {
                                r.on_hover_text(h.as_str());
                            }
                        }
                        sparkline(g, values, accent);
                        g.end_row();
                    }
                });
        });
    });
    p.add_space(ui::SPACE_SM);
}

fn plain(s: impl ToString) -> Cell {
    (s.to_string(), None)
}

fn timed(v: &PassesView, raw: u64) -> Cell {
    let (text, hover) = v.duration(raw);
    (text, Some(hover))
}

pub fn page(p: &mut egui::Ui, v: &PassesView, drawn: &mut Drawn) {
    ui::page_header(p, "Passes", "RSX time attributed by AUTO tag and named range");

    if !v.has_tags() {
        drawn.empty = true;
        drawn.empty_message = EMPTY_NOTICE.to_string();
        ui::empty_state(p, EMPTY_NOTICE, EMPTY_DETAIL);
        return;
    }

    let pal = ui::Palette::of(p);

    let tag_rows: Vec<(String, Vec<Cell>, Vec<u64>)> = v
        .tags
        .iter()
        .map(|(tag, &(frames_n, ops, busy_sum, med, p95))| {
            let per_frame = v.by.frames.iter().filter_map(|f| f.tags.get(tag).map(|r| r.busy_raw)).collect();
            (
                format!("{tag:#x}"),
                vec![plain(frames_n), plain(ops), timed(v, busy_sum), timed(v, med), timed(v, p95)],
                per_frame,
            )
        })
        .collect();
    group_card(
        p,
        "Per tag",
        "Frames, operations, RSX busy time (sum, per-frame median and p95). Hover for raw ticks.",
        &["tag", "frames", "ops", "busy sum", "median", "p95"],
        &tag_rows,
        pal.accent,
        &mut drawn.tag_rows,
    );

    let range_rows: Vec<(String, Vec<Cell>, Vec<u64>)> = v
        .ranges
        .iter()
        .map(|(name, &(frames_n, count, raw_sum, med, p95))| {
            let per_frame = v.by.frames.iter().filter_map(|f| f.ranges.get(name).map(|r| r.raw)).collect();
            (
                name.clone(),
                vec![plain(frames_n), plain(count), timed(v, raw_sum), timed(v, med), timed(v, p95)],
                per_frame,
            )
        })
        .collect();
    group_card(
        p,
        "Per range",
        "Frames, ranges, RSX time (sum, per-frame median and p95). Hover for raw ticks.",
        &["range", "frames", "ranges", "sum", "median", "p95"],
        &range_rows,
        pal.success,
        &mut drawn.range_rows,
    );

    // Only frames with something to show get a row: a capture has thousands of frames.
    let quiet = v.by.frames.iter().filter(|f| f.untimed == 0 && f.unrepresented == 0).count();
    let frame_rows: Vec<(String, Vec<Cell>, Vec<u64>)> = v
        .by
        .frames
        .iter()
        .filter(|f| f.untimed != 0 || f.unrepresented != 0)
        .map(|f| {
            (
                format!("#{}", f.frame),
                vec![plain(f.untimed), plain(f.unrepresented)],
                vec![f.untimed, u64::from(f.unrepresented)],
            )
        })
        .collect();
    group_card(
        p,
        "Frames with untimed draws",
        &format!("Draws covered but not timed, per frame; {quiet} other frame(s) have none."),
        &["frame", "untimed", "unrepresented"],
        &frame_rows,
        pal.warning,
        &mut drawn.frame_rows,
    );
}

#[cfg(test)]
mod tests {
    use super::sparkline_columns;

    #[test]
    fn long_series_fits_and_keeps_a_late_spike() {
        let mut v = vec![0u64; 1100];
        v[1099] = 500;
        v[10] = 7;
        let cols = sparkline_columns(&v, 100);
        assert_eq!(cols.len(), 100, "one column per pixel");
        assert_eq!(*cols.last().unwrap(), 500, "the last frame's spike survives");
        assert_eq!(cols[0], 7, "an early non-zero survives");
        assert!(cols[1..99].iter().all(|&c| c == 0), "zero columns stay zero");
    }

    #[test]
    fn short_series_is_unchanged() {
        assert_eq!(sparkline_columns(&[1, 0, 3], 100), vec![1, 0, 3]);
        assert_eq!(sparkline_columns(&[], 100), Vec::<u64>::new());
    }
}
