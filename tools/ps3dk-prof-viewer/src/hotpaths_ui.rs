//! The Hot paths page, drawn from the [`crate::hotpaths`] view model.  Every refusal and count is visible text.

use eframe::egui;
use ps3dk_ui as ui;

use crate::foreign_hot::{FHotView, FLaneView, ForeignHotSlot};
use crate::hotpaths::{headline, Headline, HotView, LaneKind, CAPTION};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::Analysis;

/// The page's own state (owned by the App).
#[derive(Debug, Clone, Default)]
pub struct HotState {
    pub lane: Option<u32>,
    /// The selected row's rank (1-based) on the selected lane.
    pub row: Option<usize>,
    /// An SPU/RSX lane selected instead of a PPU lane (F3b).
    pub foreign: Option<SourceKey>,
    /// The SPU coordinate premise for the SPU/RSX lanes (page-local, off by default).
    pub w32: bool,
}

/// Sets the SPU premise; a change clears the selected row (its rank belongs to the other premise's ranking).
pub fn apply_premise(st: &mut HotState, w32: bool) {
    if st.w32 != w32 {
        st.w32 = w32;
        st.row = None;
    }
}

/// Draws the page; returns a frame to open on the Frames page when a navigable link was clicked.  `foreign` adds the
/// SPU/RSX lanes (F3b) for an extension capture; their view is cached per (Document, W32).
pub fn draw(
    p: &mut egui::Ui,
    hv: &HotView,
    st: &mut HotState,
    foreign: Option<(&Analysis, &ForeignHotSlot)>,
) -> Option<(u32, u64)> {
    // The header caption follows the selected lane kind (PPU spans, or SPU/RSX completed pairs).
    ui::page_header(p, "Hot paths", if st.foreign.is_some() { crate::foreign_hot::CAPTION } else { CAPTION });
    let foreign = foreign.filter(|(a, _)| a.capture.ext.is_some());
    // The premise toggle is applied BEFORE the view is taken, so a click never shows old bounds beside the new premise.
    if foreign.is_some() {
        p.horizontal_wrapped(|h| {
            h.label(egui::RichText::new("SPU/RSX").strong());
            let mut w32 = st.w32;
            h.checkbox(&mut w32, "W32");
            apply_premise(st, w32);
            h.label(crate::foreign::W32_MEANING);
        });
    }
    let fv: Option<std::sync::Arc<FHotView>> = foreign.map(|(a, s)| s.view(a, st.w32));
    let has_foreign = fv.as_ref().is_some_and(|v| !v.lanes.is_empty() || v.notice.is_some());
    if let Some(v) = &fv {
        if let Some(n) = &v.notice {
            ui::field_message(p, ui::Tone::Warning, n);
        }
        p.horizontal_wrapped(|h| {
            for l in &v.lanes {
                if h.selectable_label(st.foreign == Some(l.source), &l.label).clicked() {
                    st.foreign = Some(l.source);
                    st.row = None;
                }
            }
            if st.foreign.is_some() && h.button("back to PPU lanes").clicked() {
                st.foreign = None;
                st.row = None;
            }
        });
        if let Some(k) = st.foreign {
            match v.lanes.iter().find(|l| l.source == k) {
                Some(l) => draw_foreign(p, l, st),
                None => st.foreign = None,
            }
            return None;
        }
    }
    match headline(hv) {
        Some(Headline::Refused(r)) => {
            ui::field_message(p, ui::Tone::Warning, &r);
            return None;
        }
        Some(Headline::NoSpans) => {
            ui::field_message(p, ui::Tone::Neutral, if has_foreign { "No PPU spans." } else { "No PPU spans: no paths." });
            return None;
        }
        None => {}
    }
    if let Some(e) = &hv.exhausted {
        ui::field_message(p, ui::Tone::Warning, e);
    }
    let lane = match st.lane.filter(|l| hv.lanes.iter().any(|x| x.incarnation == *l)) {
        Some(l) => l,
        None => {
            st.lane = Some(hv.lanes[0].incarnation);
            st.row = None;
            hv.lanes[0].incarnation
        }
    };
    p.horizontal_wrapped(|h| {
        h.label(egui::RichText::new("Lane").strong());
        for l in &hv.lanes {
            if h.selectable_label(l.incarnation == lane, format!("lane {}", l.incarnation)).clicked() && l.incarnation != lane {
                st.lane = Some(l.incarnation);
                st.row = None;
            }
        }
    });
    let lh = hv.lanes.iter().find(|l| Some(l.incarnation) == st.lane)?;
    let mut nav = None;
    egui::ScrollArea::vertical().show(p, |p| match &lh.kind {
        LaneKind::Refused(r) => ui::field_message(p, ui::Tone::Warning, r),
        LaneKind::CountsOnly { reasons, population } => {
            ui::field_message(p, ui::Tone::Warning, &format!("lane {} is NOT VALID ({reasons}): counts only, no sums", lh.incarnation));
            p.add(egui::Label::new(population).wrap());
        }
        LaneKind::Paths { lines, rows, more } => {
            for l in lines {
                p.add(egui::Label::new(l).wrap());
            }
            p.add_space(ui::SPACE_MD);
            for r in rows {
                let mark = if r.hot { "HOT " } else { "" };
                let head = format!("{}{}. {} (depth {})", mark, r.rank, r.leaf, r.depth);
                let selected = st.row == Some(r.rank);
                p.horizontal(|h| {
                    h.add_space(8.0 * (r.depth.saturating_sub(1)) as f32);
                    let t = if r.hot { egui::RichText::new(head).strong() } else { egui::RichText::new(head) };
                    if h.add(egui::SelectableLabel::new(selected, t)).clicked() {
                        st.row = if selected { None } else { Some(r.rank) };
                    }
                });
                let share = r.share.map_or("share n/a".to_string(), |s| format!("{s:.1}% of retained exclusive"));
                p.add(egui::Label::new(format!("exclusive {} | inclusive {} | {share}", r.exclusive, r.inclusive)).wrap());
                if selected {
                    p.add(egui::Label::new(format!("path: {}", r.text)).wrap());
                    p.add(egui::Label::new(&r.counts).wrap());
                    match &r.links {
                        None => {
                            p.label("frame links unavailable (retention limit)");
                        }
                        Some(ls) => {
                            for l in ls {
                                match l.nav {
                                    Some(k) => {
                                        if p.link(&l.label).clicked() {
                                            nav = Some(k);
                                        }
                                    }
                                    None => {
                                        p.label(format!("{} (no frame to open)", l.label));
                                    }
                                }
                            }
                            if r.more_links > 0 {
                                p.label(format!("{} more spans", r.more_links));
                            }
                        }
                    }
                }
                p.add_space(ui::SPACE_XS);
            }
            if let Some(m) = more {
                p.label(m);
            }
        }
    });
    nav
}

/// One SPU/RSX lane: premise and caption, coverage, counts, ranked rows (exact bound and chain on selection).
fn draw_foreign(p: &mut egui::Ui, l: &FLaneView, st: &mut HotState) {
    egui::ScrollArea::vertical().show(p, |p| {
        p.label(egui::RichText::new(&l.label).strong());
        p.add(egui::Label::new(crate::foreign::PREMISE_LINE).wrap());
        for c in &l.coverage {
            p.add(egui::Label::new(c).wrap());
        }
        if let Some(r) = &l.refusal {
            ui::field_message(p, ui::Tone::Warning, r);
            return;
        }
        for x in &l.lines {
            p.add(egui::Label::new(x).wrap());
        }
        p.add_space(ui::SPACE_MD);
        for r in &l.rows {
            let selected = st.row == Some(r.rank);
            let mark = if r.partial { "PARTIAL " } else { "" };
            let head = format!("{}{}. {} (depth {})", mark, r.rank, r.leaf, r.depth);
            p.horizontal(|h| {
                h.add_space(8.0 * (r.depth.saturating_sub(1).min(12)) as f32);
                if h.add(egui::SelectableLabel::new(selected, head)).clicked() {
                    st.row = if selected { None } else { Some(r.rank) };
                }
            });
            p.add(egui::Label::new(format!("exclusive {} | inclusive {} | {}", r.exclusive, r.inclusive, r.share)).wrap());
            if !r.order.is_empty() {
                p.label(&r.order);
            }
            if selected {
                p.add(egui::Label::new(format!("path: {}", r.text)).wrap());
                p.add(egui::Label::new(format!("exact exclusive: {}", r.exclusive_exact)).wrap());
                p.add(egui::Label::new(format!("exact inclusive: {}", r.inclusive_exact)).wrap());
                p.add(egui::Label::new(&r.counts).wrap());
            }
            p.add_space(ui::SPACE_XS);
        }
        if let Some(m) = &l.more {
            p.label(m);
        }
    });
}
