//! The Frames page, drawn with egui from the [`crate::frames`] view model.  Every reason and refusal is visible text
//! on the page (never hover only); the graph's glyph kinds are explained by a legend row.

use eframe::egui::{self, Align2, FontId, Pos2, Rect, Sense, Stroke, Vec2};
use ps3dk_prof::Analysis;
use ps3dk_ui as ui;

use crate::frames::{
    frame_tables, glyph, scale, slot_at, summary, visible_slots, Budget, FramesIndex, Glyph, LaneTable, Selection,
    SlotKind, Table,
};
use crate::foreign::{self, ForeignSlot, ForeignView, Mark, VerdictView};
use crate::frames::{Owner, MEASURED_ONLY};
use crate::model::{metric_text, TimeBase};
use crate::timeline_ui::TimelineState;

/// The Frames page's own state (owned by the App).
#[derive(Debug, Clone, Default)]
pub struct FramesState {
    pub sel: Selection,
    pub budget: Option<Budget>,
    /// The selected frame's tables, built once per selection from that frame's span lists.
    pub tables: Option<((u32, u64), Vec<LaneTable>)>,
    /// The absolute slot range painted by the last repaint (the visible columns only).
    pub painted: std::ops::Range<usize>,
    /// The SPU coordinate premise W32 for the SPU/RSX lanes (page-local, off by default).
    pub w32: bool,
}

impl FramesState {
    pub fn budget(&self) -> Budget {
        self.budget.unwrap_or(Budget::Fps30)
    }

    /// Selects (owner, frame) and builds its tables.
    pub fn select(&mut self, a: &Analysis, ix: &FramesIndex, owner: u32, frame: Option<u64>) {
        self.sel = Selection { owner: Some(owner), frame };
        self.tables = frame.map(|k| ((owner, k), frame_tables(a, ix, owner, k)));
    }

    /// A different capture: the default owner, no frame, no tables (the budget choice is kept).
    pub fn reset(&mut self, ix: Option<&FramesIndex>) {
        self.sel = ix.map(Selection::fresh).unwrap_or_default();
        self.tables = None;
    }

    /// A newer snapshot of the same capture: Selection::refreshed, and the tables rebuilt from the new document.
    pub fn refresh(&mut self, a: &Analysis, ix: &FramesIndex) {
        let sel = self.sel.refreshed(ix);
        match (sel.owner, sel.frame) {
            (Some(o), k) => self.select(a, ix, o, k),
            (None, _) => self.reset(Some(ix)),
        }
    }
}

const GRAPH_H: f32 = 180.0;
const BAND_H: f32 = 10.0;
const MIN_SLOT_W: f32 = 6.0;

fn slot_text(k: &SlotKind, tb: TimeBase) -> String {
    match k {
        SlotKind::Measured(d) => format!("MEASURED {}", tb.ticks(u128::from(*d))),
        SlotKind::Unknown(why) => format!("UNKNOWN ({why})"),
        SlotKind::Refused(why) => format!("REFUSED ({why})"),
    }
}

/// What one repaint drew, for tests: the graph's screen rectangle (None without a graph) and whether the budget
/// selector and the budget line were shown.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Drawn {
    pub graph: Option<Rect>,
    pub budget_selector: bool,
    pub budget_line: bool,
}

/// Draws the page.
pub fn draw(
    p: &mut egui::Ui,
    a: &Analysis,
    ix: &FramesIndex,
    st: &mut FramesState,
    timeline: Option<&mut TimelineState>,
    foreign: Option<&ForeignSlot>,
) -> Drawn {
    let pal = ui::Palette::of(p);
    let tb = TimeBase::of(a);
    ui::page_header(
        p,
        "Frames",
        "PPU frame elapsed (FRAME k to FRAME k+1) per owner, from the reader; every statistic uses measured frames only.",
    );
    if ix.owners.is_empty() {
        ui::field_message(p, ui::Tone::Neutral, "No FRAME records: no frame graph.");
        return Drawn::default();
    }
    let owner_id = match st.sel.owner.and_then(|o| ix.owner(o)) {
        Some(o) => o.incarnation,
        None => {
            st.reset(Some(ix));
            match st.sel.owner {
                Some(o) => o,
                None => return Drawn::default(),
            }
        }
    };
    let mut drawn = Drawn::default();
    egui::ScrollArea::vertical().show(p, |p| {
        // Owner and budget.
        p.horizontal_wrapped(|h| {
            h.label(egui::RichText::new("Owner").strong());
            for o in &ix.owners {
                let label = format!(
                    "lane {} {}{}",
                    o.incarnation,
                    o.name,
                    if o.valid { "" } else { " (NOT VALID)" }
                );
                if h.selectable_label(o.incarnation == owner_id, label).clicked() && o.incarnation != owner_id {
                    st.select(a, ix, o.incarnation, None);
                }
            }
        });
        let owner_id = st.sel.owner.unwrap_or(owner_id);
        let Some(o) = ix.owner(owner_id) else { return };
        if !o.valid {
            ui::field_message(p, ui::Tone::Warning, &format!("Owner {} is NOT VALID: {}", o.incarnation, o.reasons));
        }
        p.horizontal_wrapped(|h| {
            h.label(egui::RichText::new("Budget").strong());
            if ix.freq_hz.is_some() {
                let mut b = st.budget();
                let opts: Vec<(Budget, &str)> = Budget::ALL.iter().map(|b| (*b, b.label())).collect();
                if ui::segmented(h, &mut b, &opts) {
                    st.budget = Some(b);
                }
                drawn.budget_selector = true;
            } else {
                h.label("no CLOCKS chunk: budget unavailable (ticks only)");
            }
        });
        let budget = if ix.freq_hz.is_some() { st.budget() } else { Budget::None };
        let budget_ticks = match (budget.fps(), ix.freq_hz) {
            (Some(fps), Some(f)) => Some(f as f64 / fps as f64),
            _ => None,
        };

        // SPU/RSX lanes (F2b): only for an extension capture; the reader runs once per (owner, W32), the view once per
        // budget selector, never per repaint.
        let foreign = foreign.filter(|_| a.capture.ext.is_some());
        if foreign.is_some() {
            p.horizontal_wrapped(|h| {
                h.label(egui::RichText::new("SPU premise").strong());
                h.checkbox(&mut st.w32, "W32");
                h.label(foreign::W32_MEANING);
            });
        }
        let fv_ref = foreign.map(|f| f.view(a, ix, owner_id, st.w32, budget));
        let fv: Option<&ForeignView> = fv_ref.as_deref();

        // Legend.
        p.horizontal_wrapped(|h| {
            let swatch = |h: &mut egui::Ui, c: egui::Color32, glyph: &str, text: &str| {
                let (r, _) = h.allocate_exact_size(Vec2::new(18.0, 12.0), Sense::hover());
                h.painter().rect_filled(r, 2.0, c);
                if !glyph.is_empty() {
                    h.painter().text(r.center(), Align2::CENTER_CENTER, glyph, FontId::monospace(10.0), pal.text);
                }
                h.label(text);
                h.add_space(ui::SPACE_MD);
            };
            let mark = |h: &mut egui::Ui, g: Glyph, text: &str| {
                let (r, _) = h.allocate_exact_size(Vec2::new(18.0, 12.0), Sense::hover());
                status_mark(h.painter(), r, g, &pal);
                h.label(text);
                h.add_space(ui::SPACE_MD);
            };
            swatch(h, pal.accent, "", "measured");
            if let (Some(fps), Some(f)) = (budget.fps(), ix.freq_hz) {
                swatch(h, pal.danger, "", &format!("over the {} budget", budget.label()));
                let (r, _) = h.allocate_exact_size(Vec2::new(18.0, 12.0), Sense::hover());
                h.painter().line_segment([r.left_center(), r.right_center()], Stroke::new(1.5_f32, pal.danger));
                h.label(format!("budget line: freq/fps = {f}/{fps} ticks ({})", budget.label()));
                h.add_space(ui::SPACE_MD);
            }
            mark(h, Glyph::Zero, "measured 0 (solid band)");
            mark(h, Glyph::Unknown, "unknown (outline)");
            mark(h, Glyph::Refused, "refused (crossed)");
        });

        // The graph: only the slots inside the horizontal viewport are generated and painted.
        let n = o.slots.len();
        let avail = p.available_width();
        let slot_w = (avail / n.max(1) as f32).max(MIN_SLOT_W);
        let width = slot_w * n as f32;
        let sc = scale(o, ix.freq_hz, budget, f64::from(GRAPH_H - BAND_H - 4.0));
        if width > avail {
            p.label(format!("{n} frames; the graph scrolls sideways (one column per frame)"));
        }
        let mut clicked: Option<usize> = None;
        let mut painted = 0..0;
        egui::ScrollArea::horizontal().id_salt("frames-graph").show_viewport(p, |p, viewport| {
            let (rect, resp) = p.allocate_exact_size(Vec2::new(width, GRAPH_H), Sense::click());
            drawn.graph = Some(rect);
            let painter = p.painter_at(rect);
            painter.rect_filled(rect, 2.0, pal.surface);
            let base = rect.bottom() - BAND_H - 2.0;
            painted = visible_slots(n, f64::from(slot_w), f64::from(viewport.min.x), f64::from(viewport.max.x));
            for i in painted.clone() {
                let x0 = rect.left() + i as f32 * slot_w;
                let r = |y0: f32, y1: f32| Rect::from_min_max(Pos2::new(x0 + 1.0, y0), Pos2::new(x0 + slot_w - 1.0, y1));
                match glyph(o, i, &sc) {
                    Glyph::Bar { height, over } => {
                        let h = (height as f32).max(1.0);
                        painter.rect_filled(r(base - h, base), 1.0, if over { pal.danger } else { pal.accent });
                    }
                    g => status_mark(&painter, r(base - BAND_H, base), g, &pal),
                }
                if st.sel.frame == Some(o.slots[i].frame) {
                    painter.rect_stroke(r(rect.top() + 2.0, rect.bottom() - 2.0), 1.0, Stroke::new(1.5_f32, pal.text));
                }
            }
            if let Some(y) = sc.line {
                drawn.budget_line = true;
                let y = base - y as f32;
                painter.line_segment(
                    [Pos2::new(rect.left(), y), Pos2::new(rect.right(), y)],
                    Stroke::new(1.5_f32, pal.danger),
                );
            }
            if resp.clicked() {
                if let Some(pos) = resp.interact_pointer_pos() {
                    clicked = slot_at(n, f64::from(slot_w), f64::from(pos.x - rect.left()));
                }
            }
            if let Some(v) = fv {
                let strips = Strips { o, slot_w, width, budget_ticks, selected: st.sel.frame };
                strips.draw(p, v, viewport, &painted, &pal);
            }
        });
        st.painted = painted;
        if let Some(i) = clicked {
            let s = &o.slots[i];
            st.select(a, ix, owner_id, Some(s.frame));
            if let (Some(next), Some(tl)) = (s.next_tb, timeline) {
                tl.view.show_range(&tl.tl, s.tb, next);
            }
        }

        // Summary (measured frames only).
        let sm = summary(o, ix.freq_hz, budget);
        p.add_space(ui::SPACE_LG);
        ui::section_label(p, &format!("SUMMARY ({MEASURED_ONLY})"));
        p.label(format!(
            "{} measured; {} unknown and {} refused excluded",
            sm.measured, sm.unknown, sm.refused
        ));
        let some = |v: Option<u64>| v.map_or("none".to_string(), |d| tb.ticks(u128::from(d)));
        p.label(format!("min {} | max {}", some(sm.min), some(sm.max)));
        p.label(format!(
            "p50 {} | p90 {}",
            metric_text(&sm.p50, |d| tb.ticks(u128::from(*d))),
            metric_text(&sm.p90, |d| tb.ticks(u128::from(*d)))
        ));
        if let Some(m) = sm.mean {
            p.label(format!("mean of {} measured frames: {}", sm.measured, tb.ticks_f(m)));
        }
        if let (Some(over), Some(_)) = (sm.over, budget.fps()) {
            p.label(format!("over the {} budget: {over} of {} measured", budget.label(), sm.measured));
        }

        if let Some(v) = fv {
            draw_lane_text(p, v);
        }

        // The selected frame.
        p.add_space(ui::SPACE_LG);
        ui::section_label(p, "SELECTED FRAME");
        let Some(k) = st.sel.frame else {
            p.label("Click a frame in the graph.");
            draw_unattributed(p, ix);
            return;
        };
        let Some((_, s)) = o.slot_of(k) else { return };
        p.add(egui::Label::new(format!("Frame {k}: {}", slot_text(&s.kind, tb))).wrap());
        if let (SlotKind::Measured(d), Some(fps), Some(f)) = (&s.kind, budget.fps(), ix.freq_hz) {
            if crate::frames::over_budget(*d, fps, f) {
                ui::field_message(p, ui::Tone::Danger, &format!("over the {} budget", budget.label()));
            }
        }
        p.add(
            egui::Label::new(match s.next_tb {
                Some(_) => format!(
                    "Timeline framed on FRAME {k} to FRAME {} (with the Timeline's own margin and clamp).",
                    k + 1
                ),
                None => "Timeline not moved: this frame has no measured interval.".to_string(),
            })
            .wrap(),
        );
        if let Some((_, tables)) = &st.tables {
            if tables.is_empty() {
                p.label("No PPU lane has attributed work in this frame.");
            } else {
                let lanes: Vec<String> = tables.iter().map(|t| t.lane.to_string()).collect();
                p.label(format!("PPU lanes with attributed work: {}", lanes.join(", ")));
            }
            for t in tables {
                draw_table(p, a, t, tb);
            }
        }
        if let Some(v) = fv {
            draw_lane_tables(p, v, o, k);
        }
        draw_unattributed(p, ix);
    });
    drawn
}

const STRIP_H: f32 = 48.0;
/// The label row at the top of each strip; bars use the rest.
const STRIP_LABEL_H: f32 = 14.0;

/// The SPU/RSX lane strips under the PPU graph: same slot columns and scroll, one shared y scale (`ForeignView::top`).
struct Strips<'o> {
    o: &'o Owner,
    slot_w: f32,
    width: f32,
    budget_ticks: Option<f64>,
    selected: Option<u64>,
}

impl Strips<'_> {
    fn draw(&self, p: &mut egui::Ui, v: &ForeignView, viewport: Rect, painted: &std::ops::Range<usize>, pal: &ui::Palette) {
        let span = STRIP_H - STRIP_LABEL_H - 4.0;
        for lane in &v.lanes {
            p.add_space(2.0);
            let (rect, _) = p.allocate_exact_size(Vec2::new(self.width, STRIP_H), Sense::hover());
            let painter = p.painter_at(rect);
            painter.rect_filled(rect, 2.0, pal.surface);
            let base = rect.bottom() - 2.0;
            // Pixels only: the ratio is clamped, so a huge or non-finite value stays inside the strip.
            let y = |t: f64| -> f32 {
                let f = if t.is_finite() { (t / v.top).clamp(0.0, 1.0) } else { 1.0 };
                base - f as f32 * span
            };
            let top_y = base - span;
            for i in painted.clone() {
                let x0 = rect.left() + i as f32 * self.slot_w;
                let cx = x0 + self.slot_w / 2.0;
                match foreign::mark(self.o, lane, i) {
                    Mark::Bar(b) => {
                        let lo = y(b.lo);
                        let hi = b.hi.map_or(top_y, y);
                        let r = Rect::from_min_max(
                            Pos2::new(x0 + 1.0, hi.min(lo - 2.0)),
                            Pos2::new(x0 + self.slot_w - 1.0, lo),
                        );
                        let c = match b.verdict {
                            VerdictView::Over => pal.danger,
                            VerdictView::Within => pal.accent,
                            VerdictView::Undetermined => pal.warning,
                            _ => pal.text_muted,
                        };
                        // Faint from 0 to lo (the bound's guaranteed part), solid from lo to hi (its uncertainty).
                        let under =
                            Rect::from_min_max(Pos2::new(x0 + 1.0, r.bottom()), Pos2::new(x0 + self.slot_w - 1.0, base));
                        painter.rect_filled(under, 0.0, c.gamma_multiply(0.3));
                        painter.rect_filled(r, 1.0, c);
                        if b.verdict == VerdictView::Undetermined {
                            let mut yy = r.top() + 2.0;
                            while yy < r.bottom() {
                                painter.line_segment(
                                    [Pos2::new(r.left(), yy), Pos2::new(r.right(), yy)],
                                    Stroke::new(1.0_f32, pal.text),
                                );
                                yy += 3.0;
                            }
                        }
                        if b.hi_inf {
                            // the open-arrow cap: +inf
                            let st = Stroke::new(1.5_f32, pal.text);
                            painter.line_segment([Pos2::new(cx - 3.0, top_y + 3.0), Pos2::new(cx, top_y)], st);
                            painter.line_segment([Pos2::new(cx + 3.0, top_y + 3.0), Pos2::new(cx, top_y)], st);
                        }
                        if b.lo_open {
                            painter.circle_stroke(Pos2::new(cx, lo), 2.0, Stroke::new(1.0_f32, pal.text));
                        }
                    }
                    Mark::NonePlaced => {
                        painter.circle_filled(Pos2::new(cx, base - 2.0), 1.5, pal.text_muted);
                    }
                    Mark::NoInterval => {}
                }
                if self.selected == Some(self.o.slots[i].frame) {
                    let r = Rect::from_min_max(Pos2::new(x0 + 1.0, rect.top() + 1.0), Pos2::new(x0 + self.slot_w - 1.0, rect.bottom() - 1.0));
                    painter.rect_stroke(r, 1.0, Stroke::new(1.5_f32, pal.text));
                }
            }
            if let Some(t) = self.budget_ticks {
                let yb = y(t);
                painter.line_segment([Pos2::new(rect.left(), yb), Pos2::new(rect.right(), yb)], Stroke::new(1.0_f32, pal.danger));
            }
            painter.text(
                Pos2::new(rect.left() + viewport.min.x + 4.0, rect.top() + 1.0),
                Align2::LEFT_TOP,
                &lane.label,
                FontId::proportional(11.0),
                pal.text,
            );
        }
    }
}

/// The lanes' always-visible text: premise, refusal or state, legend, and per lane its epoch rows and counts.
fn draw_lane_text(p: &mut egui::Ui, v: &ForeignView) {
    p.add_space(ui::SPACE_LG);
    ui::section_label(p, "SPU/RSX LANES");
    p.add(egui::Label::new(foreign::PREMISE_LINE).wrap());
    if let Some(r) = &v.refusal {
        ui::field_message(p, ui::Tone::Warning, r);
        return;
    }
    if let Some(s) = &v.state {
        ui::field_message(p, ui::Tone::Neutral, s);
    }
    p.add(
        egui::Label::new(
            "strips: one range bar per frame from lo to hi (arrow cap: +inf; hollow cap: open lower end); red OVER, \
             blue WITHIN, amber striped UNDETERMINED, grey no verdict; the faint part below a bar is its guaranteed \
             lower bound; dot: measured frame, nothing included on this lane; one scale for every strip",
        )
        .wrap(),
    );
    for lane in &v.lanes {
        p.add_space(ui::SPACE_MD);
        p.label(egui::RichText::new(&lane.label).strong());
        for e in &lane.epochs {
            p.add(egui::Label::new(e).wrap());
        }
        for e in &lane.errors {
            ui::field_message(p, ui::Tone::Warning, e);
        }
        for f in &lane.footer {
            p.add(egui::Label::new(f).wrap());
        }
    }
}

/// The selected frame's per-lane rows: the bar (exact), its verdict, the zone rows (exclusive, exact) and the counts.
fn draw_lane_tables(p: &mut egui::Ui, v: &ForeignView, o: &Owner, k: u64) {
    if v.refusal.is_some() || v.lanes.is_empty() {
        return;
    }
    let Some((i, _)) = o.slot_of(k) else { return };
    for lane in &v.lanes {
        p.add_space(ui::SPACE_MD);
        p.label(egui::RichText::new(format!("{} (frame {k})", lane.label)).strong());
        let Some(c) = lane.cells.get(&i) else {
            p.label(match foreign::mark(o, lane, i) {
                Mark::NoInterval => "no measured interval for this frame: nothing placed",
                _ => "none placed on this lane",
            });
            continue;
        };
        match (&c.bar, &c.bar_exact) {
            (Some(b), Some(exact)) => {
                p.add(egui::Label::new(foreign::BAR_CAPTION).wrap());
                p.add(egui::Label::new(format!("bar {}", b.label)).wrap());
                p.add(egui::Label::new(format!("exact: {exact}")).wrap());
                if c.bar_epochs > 1 {
                    p.label(format!("spans {} epochs (interval sum)", c.bar_epochs));
                }
                p.label(b.verdict.text());
            }
            _ => {
                p.label("none placed on this lane (nothing included)");
            }
        }
        if !c.rows.is_empty() {
            p.add(egui::Label::new(format!("{} {}", ps3dk_prof::report::W_NOTE, foreign::ZONE_NOTE)).wrap());
            // One stacked block per zone (wrapped text, never wider than the page).
            for r in &c.rows {
                p.add_space(ui::SPACE_XS);
                p.add(egui::Label::new(egui::RichText::new(&r.label).strong()).wrap());
                let spans =
                    if r.epochs > 1 { format!("; spans {} epochs (interval sum)", r.epochs) } else { String::new() };
                p.add(egui::Label::new(format!("exclusive {}; {} pair(s){spans}", r.rounded, r.pairs)).wrap());
                p.add(egui::Label::new(format!("exact: {}", r.exact)).wrap());
            }
        }
        p.add(egui::Label::new(&c.counts).wrap());
    }
}

fn counts_line(t: &LaneTable) -> String {
    let c = &t.counts;
    let k = |name: &str, v: &crate::frames::KindCounts| {
        format!("{name}: {} completed, {} censored, {} unavailable", v.completed, v.censored, v.unavailable)
    };
    format!(
        "{} claimed span{} | {} | {} | {}",
        c.total(),
        if c.total() == 1 { "" } else { "s" },
        k("attributed", &c.frame),
        k("owner invalid", &c.owner_invalid),
        k("unresolved", &c.unresolved)
    )
}

fn draw_table(p: &mut egui::Ui, a: &Analysis, t: &LaneTable, tb: TimeBase) {
    p.add_space(ui::SPACE_MD);
    p.label(egui::RichText::new(format!("Lane {}", t.lane)).strong());
    p.add(egui::Label::new(counts_line(t)).wrap());
    match &t.table {
        Table::Refused(why) => {
            ui::field_message(p, ui::Tone::Warning, &format!("W REFUSED: {why}"));
            if let Some(v) = a.lanes.get(&t.lane).filter(|v| !v.valid) {
                ui::field_message(p, ui::Tone::Warning, &format!("lane {} NOT VALID: {}", t.lane, v.reasons.join(", ")));
            }
        }
        Table::Measured { w, rows } => {
            p.add(egui::Label::new(ps3dk_prof::report::W_NOTE).wrap());
            p.label(format!("W (exclusive, this lane): {}", tb.ticks(*w)));
            egui::Grid::new(("hot", t.lane)).num_columns(3).striped(true).spacing([ui::SPACE_LG, ui::SPACE_XS]).show(
                p,
                |g| {
                    for h in ["Zone", "Spans", "Exclusive"] {
                        g.label(egui::RichText::new(h).strong());
                    }
                    g.end_row();
                    for r in rows {
                        g.label(&r.label);
                        g.label(r.spans.to_string());
                        g.label(tb.ticks(r.exclusive));
                        g.end_row();
                    }
                },
            );
        }
    }
}

fn draw_unattributed(p: &mut egui::Ui, ix: &FramesIndex) {
    if ix.unattributed.is_empty() {
        return;
    }
    p.add_space(ui::SPACE_LG);
    ui::section_label(p, "UNATTRIBUTED (no frame claim; whole capture)");
    for (lane, u) in &ix.unattributed {
        p.label(format!(
            "Lane {lane}: {} before the first FRAME, {} after a loss, {} on a lane without FRAMEs",
            u.pre_frame, u.loss, u.not_owner
        ));
    }
}

/// A status mark in `r`: measured 0 a solid band, UNKNOWN an outline, REFUSED a band with an X (shape-coded, so the
/// kinds stay distinct at narrow widths and without colour).
fn status_mark(painter: &egui::Painter, r: Rect, g: Glyph, pal: &ui::Palette) {
    match g {
        Glyph::Zero => {
            painter.rect_filled(r, 1.0, pal.success);
        }
        Glyph::Unknown => {
            painter.rect_stroke(r.shrink(0.5), 1.0, Stroke::new(1.0_f32, pal.text_muted));
        }
        Glyph::Refused => {
            painter.rect_filled(r, 1.0, pal.warning.gamma_multiply(0.45));
            let st = Stroke::new(1.0_f32, pal.text);
            painter.line_segment([r.left_top(), r.right_bottom()], st);
            painter.line_segment([r.left_bottom(), r.right_top()], st);
        }
        Glyph::Bar { .. } => {}
    }
}
