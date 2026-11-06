//! The RSX frames page: the captures found in the data (state badge, reasons, draw count), the selected capture's
//! draw list (ordinal or UNKNOWN, kind, raw GPU begin/end), and every reserved-marker anomaly.  The list is a
//! scrolling table that builds only the visible rows, clamped to [`crate::rsx::PAINT_ROWS`] per repaint, with an
//! omission line when anything is not shown.  No request button: this page lists what the file holds.

use eframe::egui::{self, Rect, RichText};
use ps3dk_prof::rsx_capture::CaptureState;
use ps3dk_prof::Analysis;
use ps3dk_ui as ui;

use crate::rsx::{RsxView, PAINT_ROWS};

pub const TITLE: &str = "RSX frames";
const ROW_H: f32 = 18.0;
const LIST_H: f32 = 360.0;

/// The page's selection.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RsxState {
    pub selected: Option<u32>,
}

/// What the page drew (for tests).
#[derive(Debug, Clone, Default)]
pub struct Drawn {
    /// Each capture row's cap and rect.
    pub items: Vec<(u32, Rect)>,
    /// The selected capture's state line.
    pub state_text: Option<String>,
    /// Draw rows built this repaint, and the list's omission line.
    pub rows_built: usize,
    pub omitted: Option<String>,
    pub anomalies: usize,
    /// The excess-caps line, as drawn.
    pub excess: Option<String>,
}

pub fn tone(s: CaptureState) -> ui::Tone {
    match s {
        CaptureState::Finalized => ui::Tone::Success,
        CaptureState::Partial => ui::Tone::Warning,
        CaptureState::Anomaly => ui::Tone::Danger,
    }
}

/// The page.  A selection that no longer names a capture of this view shows nothing selected.  `drawn` is reset
/// first (egui may run a frame's closure more than once).
pub fn page(p: &mut egui::Ui, a: &Analysis, v: &RsxView, st: &mut RsxState, drawn: &mut Drawn) {
    *drawn = Drawn::default();
    ui::page_header(
        p,
        TITLE,
        "RSX frame captures found in this file, as the reader classified their markers. Raw RSX timestamps; no PPU time mapping on this page.",
    );
    if v.caps.captures.is_empty() && v.caps.not_analysed.is_empty() && v.caps.anomalies.is_empty() {
        ui::field_message(p, ui::Tone::Neutral, "No RSX frame capture in this file.");
        return;
    }
    egui::ScrollArea::vertical().id_salt("rsx-page").show(p, |p| {
        ui::section_label(p, "CAPTURES");
        egui::Grid::new("rsx-captures").num_columns(5).striped(true).spacing([ui::SPACE_LG, ui::SPACE_XS]).show(p, |g| {
            for h in ["Cap", "Frame", "State", "Draws", "Source"] {
                g.label(RichText::new(h).strong());
            }
            g.end_row();
            for r in &v.rows {
                let resp = g.selectable_label(st.selected == Some(r.cap), format!("cap {}", r.cap));
                drawn.items.push((r.cap, resp.rect));
                if resp.clicked() {
                    st.selected = Some(r.cap);
                }
                g.label(&r.frame);
                ui::badge(g, r.state.word(), tone(r.state)).on_hover_text(&r.line);
                g.label(&r.draws);
                g.label(&r.source);
                g.end_row();
            }
        });
        if let Some(line) = &v.excess_line {
            ui::field_message(p, ui::Tone::Warning, line);
            drawn.excess = Some(line.clone());
        }
        if let Some(ci) = st.selected.and_then(|cap| v.caps.captures.iter().position(|c| c.cap == cap)) {
            selected(p, a, v, ci, drawn);
        }
        if v.caps.anomaly_count() > 0 {
            p.add_space(ui::SPACE_LG);
            ui::section_label(p, "ANOMALIES");
            for line in &v.anomaly_lines {
                ui::field_message(p, ui::Tone::Danger, line);
            }
            drawn.anomalies = v.caps.anomalies.len();
        }
    });
}

fn selected(p: &mut egui::Ui, a: &Analysis, v: &RsxView, ci: usize, drawn: &mut Drawn) {
    let c = &v.caps.captures[ci];
    let r = &v.rows[ci];
    p.add_space(ui::SPACE_LG);
    ui::section_label(p, &format!("CAP {}", c.cap));
    ui::field_message(p, tone(c.state), &r.line);
    drawn.state_text = Some(r.line.clone());
    if let Some(b) = c.begin {
        p.label(format!(
            "conn {} · wire {} · dom {} · owner {} · frame {} · BEGIN seq {}",
            b.tag.conn, b.tag.wire, b.tag.dom, b.tag.owner, b.tag.frame, b.seq
        ));
    }
    if let Some(e) = c.end {
        p.label(format!(
            "END seq {} · hooks {} · other ctx {}{} · other thread {}{} (outside this capture's binding; not blocking)",
            e.seq,
            e.tag.hooks,
            e.tag.other_ctx,
            if e.tag.other_ctx_sat { "+" } else { "" },
            e.tag.other_thread,
            if e.tag.other_thread_sat { "+" } else { "" }
        ));
    }
    let exact = c.segments.iter().filter(|s| s.exact).count();
    p.label(format!(
        "{} block segment(s): {exact} exact, {} UNKNOWN · {} checkpoint(s) received",
        c.segments.len(),
        c.segments.len() - exact,
        c.checkpoints.len()
    ));
    let (n, beyond) = v.listed(ci);
    if c.draws.is_empty() && c.draw_pairs > 0 {
        drawn.omitted = Some(format!(
            "{} draw pairs not listed: the capture was refused (over the cap or the file's draw budget)",
            c.draw_pairs
        ));
    } else if c.attribution_unknown {
        drawn.omitted = Some("Draws UNKNOWN: BEGIN and every checkpoint are missing, so no draw can be attributed.".into());
    }
    if n == 0 {
        if let Some(o) = &drawn.omitted {
            ui::field_message(p, ui::Tone::Warning, o);
        } else {
            p.label("No draws.");
        }
        return;
    }
    p.add_space(ui::SPACE_SM);
    let cols = [70.0, 90.0, 110.0, 170.0, 170.0];
    let header = |h: &mut egui::Ui| {
        for (t, w) in ["#", "Ordinal", "Kind", "GPU begin (raw)", "GPU end (raw)"].iter().zip(cols) {
            h.add_sized([w, ROW_H], egui::Label::new(RichText::new(*t).strong()));
        }
    };
    p.horizontal(header);
    let mut clamped = 0usize;
    egui::ScrollArea::vertical().id_salt(("rsx-draws", c.cap)).max_height(LIST_H).show_rows(p, ROW_H, n, |p, range| {
        let shown = range.start..range.end.min(range.start + PAINT_ROWS);
        clamped = range.len() - shown.len();
        for i in shown {
            let Some(d) = v.draw_row(a, ci, i) else { continue };
            drawn.rows_built += 1;
            p.horizontal(|h| {
                let unknown = d.ordinal == "UNKNOWN";
                for (t, w) in [d.index.to_string(), d.ordinal, d.kind, d.begin_raw, d.end_raw].into_iter().zip(cols) {
                    let mut text = RichText::new(t);
                    if unknown {
                        text = text.color(ui::Palette::of(h).warning);
                    }
                    h.add_sized([w, ROW_H], egui::Label::new(text));
                }
            });
        }
    });
    if clamped > 0 {
        drawn.omitted = Some(format!("{clamped} row(s) in view not drawn (the per-repaint row cap of {PAINT_ROWS})"));
    } else if beyond > 0 {
        drawn.omitted = Some(format!("{beyond} draw row(s) beyond the list cap not shown"));
    }
    if let Some(o) = &drawn.omitted {
        ui::field_message(p, ui::Tone::Warning, o);
    }
}
