//! The Hot paths page, drawn from the [`crate::hotpaths`] view model.  Every refusal and count is visible text.

use eframe::egui;
use ps3dk_ui as ui;

use crate::hotpaths::{headline, Headline, HotView, LaneKind, CAPTION};

/// The page's own state (owned by the App).
#[derive(Debug, Clone, Default)]
pub struct HotState {
    pub lane: Option<u32>,
    /// The selected row's rank (1-based) on the selected lane.
    pub row: Option<usize>,
}

/// Draws the page; returns a frame to open on the Frames page when a navigable link was clicked.
pub fn draw(p: &mut egui::Ui, hv: &HotView, st: &mut HotState) -> Option<(u32, u64)> {
    ui::page_header(p, "Hot paths", CAPTION);
    match headline(hv) {
        Some(Headline::Refused(r)) => {
            ui::field_message(p, ui::Tone::Warning, &r);
            return None;
        }
        Some(Headline::NoSpans) => {
            ui::field_message(p, ui::Tone::Neutral, "No PPU spans: no paths.");
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
