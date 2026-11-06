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
use crate::graphs::{self, GraphMode, LinePoint, Smoothing};
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
    /// The graph display mode (in memory for this viewer; kept across captures like the budget).
    pub mode: GraphMode,
    /// The lines mode's moving-average window (raw lines are always drawn).
    pub smoothing: Smoothing,
    /// Lines and distribution: scale to the maximum (true) or to the p99 fit (false, the default).
    pub full_scale: bool,
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
    /// The mode drawn (Bars when no graph was drawn).
    pub mode: GraphMode,
    /// The per-slot (or per-bin) evaluations of a non-bar mode in this repaint: bounded by the visible columns.
    pub work: usize,
    /// Lanes whose distribution was refused, shown as a Fit p99 fallback note (Lines and Distribution).
    pub fit_fallback: usize,
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

        // Graph mode (and the lines mode's smoothing).
        p.horizontal_wrapped(|h| {
            h.label(egui::RichText::new("Graph").strong());
            let opts: Vec<(GraphMode, &str)> = GraphMode::ALL.iter().map(|m| (*m, m.label())).collect();
            ui::segmented(h, &mut st.mode, &opts);
            if st.mode == GraphMode::Lines {
                h.add_space(ui::SPACE_MD);
                h.label(egui::RichText::new("Smoothing").strong());
                let opts: Vec<(Smoothing, &str)> = Smoothing::ALL.iter().map(|m| (*m, m.label())).collect();
                ui::segmented(h, &mut st.smoothing, &opts);
            }
            if matches!(st.mode, GraphMode::Lines | GraphMode::Distribution) {
                h.add_space(ui::SPACE_MD);
                h.label(egui::RichText::new("Scale").strong());
                ui::segmented(h, &mut st.full_scale, &[(false, "Fit p99"), (true, "Max")]);
            }
        });
        let mode = st.mode;
        let lanes: &[foreign::LaneView] = fv.filter(|v| v.refusal.is_none()).map_or(&[], |v| &v.lanes[..]);
        // The lane distribution: only the modes that show it (Distribution, and the Lines p99 fit) ever build it, once
        // per reader entry and metered; Bars never asks.
        let want_dist = mode == GraphMode::Distribution || (mode == GraphMode::Lines && !st.full_scale);
        let dist_ref = foreign.filter(|_| want_dist && !lanes.is_empty()).and_then(|f| f.dist(ix, owner_id, st.w32));
        let dist: &[Result<foreign::LaneDist, String>] = dist_ref.as_deref().map_or(&[], |d| &d.lanes[..]);
        if mode != GraphMode::Bars {
            mode_legend(p, mode, st.smoothing, lanes, budget, ix.freq_hz, &pal);
            if want_dist {
                let labels: Vec<&str> = lanes.iter().map(|l| l.label.as_str()).collect();
                let notes = graphs::fit_fallback_notes(&labels, dist, st.full_scale);
                drawn.fit_fallback = notes.len();
                for n in &notes {
                    ui::field_message(p, ui::Tone::Warning, n);
                }
            }
        } else {
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
        }

        // The graph: only the slots inside the horizontal viewport are generated and painted.
        let n = o.slots.len();
        let avail = p.available_width();
        let slot_w = (avail / n.max(1) as f32).max(MIN_SLOT_W);
        let width = slot_w * n as f32;
        let sc = scale(o, ix.freq_hz, budget, f64::from(GRAPH_H - BAND_H - 4.0));
        if width > avail && mode.per_frame() {
            p.label(format!("{n} frames; the graph scrolls sideways (one column per frame)"));
        }
        let mut clicked: Option<usize> = None;
        let mut painted = 0..0;
        drawn.mode = mode;
        let cx = ModeCtx {
            o,
            lanes,
            freq_hz: ix.freq_hz,
            budget,
            budget_ticks,
            slot_w,
            width,
            selected: st.sel.frame,
            smoothing: st.smoothing,
            top: graphs::fit_top(o, dist, fv.filter(|v| v.refusal.is_none()).map(|v| v.top), budget_ticks, st.full_scale),
            dist,
            full_scale: st.full_scale,
            fallback: drawn.fit_fallback,
            tb,
        };
        if mode == GraphMode::Distribution {
            drawn.work = draw_distribution(p, &cx, tb, &pal);
            drawn.budget_line = budget_ticks.is_some();
            // No frame columns here: the last painted range (and the scroll offset egui keeps) stay for the
            // per-frame modes.
            painted = st.painted.clone();
        } else {
            egui::ScrollArea::horizontal().id_salt("frames-graph").show_viewport(p, |p, viewport| {
                if mode != GraphMode::Bars {
                    let out = match mode {
                        GraphMode::Lines => draw_lines(p, &cx, viewport, &pal),
                        _ => draw_verdict_map(p, &cx, viewport, &pal),
                    };
                    drawn.graph = Some(out.rect);
                    drawn.work = out.work;
                    drawn.budget_line = out.budget_line;
                    painted = out.painted;
                    if out.clicked {
                        clicked = out.at.and_then(|x| slot_at(n, f64::from(slot_w), f64::from(x - out.rect.left())));
                    }
                    return;
                }
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
        }
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

// ---- graph display modes (crate::graphs) ------------------------------------------------------------------------

const LINES_H: f32 = 220.0;
const MAP_ROW_H: f32 = 14.0;
const DIST_PPU_H: f32 = 110.0;
const DIST_PCT_H: f32 = 9.0;

/// The lane line colours: a colour-blind-safe set (Okabe-Ito orange, bluish green and reddish purple), chosen to
/// stay apart from the PPU's accent blue and the budget's red; cycled with a dash pattern (`graphs::lane_style`).
pub const LANE_COLOURS: [egui::Color32; graphs::LANE_COLOURS] = [
    egui::Color32::from_rgb(0xe6, 0x9f, 0x00),
    egui::Color32::from_rgb(0x00, 0x9e, 0x73),
    egui::Color32::from_rgb(0xcc, 0x79, 0xa7),
];

fn lane_colour(j: usize) -> (egui::Color32, graphs::Dash) {
    let (c, d) = graphs::lane_style(j);
    (LANE_COLOURS[c], d)
}

/// What a non-bar mode needs from the page (the same facts the bar view uses).
struct ModeCtx<'a> {
    o: &'a Owner,
    lanes: &'a [foreign::LaneView],
    /// Aligned with `lanes` when built (Distribution, Lines p99 fit); empty otherwise.
    dist: &'a [Result<foreign::LaneDist, String>],
    freq_hz: Option<u64>,
    budget: Budget,
    budget_ticks: Option<f64>,
    slot_w: f32,
    width: f32,
    selected: Option<u64>,
    smoothing: Smoothing,
    /// The shared value-axis top in ticks ([`graphs::fit_top`]).
    top: f64,
    full_scale: bool,
    /// Lanes whose refused distribution makes the Fit p99 top fall back to their finite maximum.
    fallback: usize,
    tb: TimeBase,
}

struct ModeOut {
    rect: Rect,
    clicked: bool,
    at: Option<f32>,
    painted: std::ops::Range<usize>,
    work: usize,
    budget_line: bool,
}

/// A polyline with the series' dash pattern.
fn polyline(painter: &egui::Painter, pts: Vec<Pos2>, stroke: Stroke, dash: graphs::Dash) {
    match dash {
        graphs::Dash::Solid => {
            painter.add(egui::Shape::line(pts, stroke));
        }
        graphs::Dash::Dashed => {
            painter.extend(egui::Shape::dashed_line(&pts, stroke, 6.0, 4.0));
        }
        graphs::Dash::Dotted => {
            painter.extend(egui::Shape::dotted_line(&pts, stroke.color, 4.0, stroke.width * 0.75));
        }
    }
}

/// The swatch of one series in a legend: a short line in its colour and dash.
fn series_swatch(h: &mut egui::Ui, c: egui::Color32, dash: graphs::Dash, width: f32, text: &str) {
    let (r, _) = h.allocate_exact_size(Vec2::new(24.0, 12.0), Sense::hover());
    polyline(h.painter(), vec![r.left_center(), r.right_center()], Stroke::new(width, c), dash);
    h.label(text);
    h.add_space(ui::SPACE_MD);
}

/// The legend and captions of a non-bar mode (always visible text, never hover only).
fn mode_legend(
    p: &mut egui::Ui,
    mode: GraphMode,
    smoothing: Smoothing,
    lanes: &[foreign::LaneView],
    budget: Budget,
    freq_hz: Option<u64>,
    pal: &ui::Palette,
) {
    let series = |h: &mut egui::Ui| {
        series_swatch(h, pal.accent, graphs::Dash::Solid, 2.0, "PPU frame elapsed (exact)");
        for (j, l) in lanes.iter().enumerate() {
            let (c, d) = lane_colour(j);
            series_swatch(h, c, d, 1.5, &l.label);
        }
    };
    let budget_text = |h: &mut egui::Ui| {
        if let (Some(fps), Some(f)) = (budget.fps(), freq_hz) {
            let (r, _) = h.allocate_exact_size(Vec2::new(18.0, 12.0), Sense::hover());
            h.painter().line_segment([r.left_center(), r.right_center()], Stroke::new(1.5_f32, pal.danger));
            h.label(format!("budget: freq/fps = {f}/{fps} ticks ({})", budget.label()));
        }
    };
    match mode {
        GraphMode::Bars => {}
        GraphMode::Lines => {
            p.horizontal_wrapped(|h| {
                series(h);
                budget_text(h);
            });
            p.add(egui::Label::new(LINES_CAPTION).wrap());
            if let Some(n) = smoothing.window() {
                p.add(
                    egui::Label::new(format!(
                        "smoothing: trailing mean of up to {n} frames within one unbroken run (fewer at the start \
                         of a run, never across a gap); the raw lines stay drawn, faint"
                    ))
                    .wrap(),
                );
            }
        }
        GraphMode::Distribution => {
            p.horizontal_wrapped(|h| {
                series(h);
                budget_text(h);
            });
            p.add(egui::Label::new(DIST_CAPTION).wrap());
        }
        GraphMode::VerdictMap => {
            p.horizontal_wrapped(|h| {
                let cell = |h: &mut egui::Ui, c: graphs::Cell, text: &str| {
                    let (r, _) = h.allocate_exact_size(Vec2::new(18.0, 12.0), Sense::hover());
                    map_cell(h.painter(), r, &c, pal);
                    h.label(text);
                    h.add_space(ui::SPACE_MD);
                };
                cell(h, graphs::Cell::Over, "OVER");
                cell(h, graphs::Cell::Within, "WITHIN");
                cell(h, graphs::Cell::Undetermined, "UNDETERMINED (striped)");
                cell(h, graphs::Cell::NoVerdict, "no verdict (no budget or clock)");
                cell(h, graphs::Cell::Unavailable, "verdict unavailable");
                cell(h, graphs::Cell::NonePlaced, "nothing included (dot)");
                cell(h, graphs::Cell::Unknown, "unknown (outline)");
                cell(h, graphs::Cell::Refused, "refused (crossed)");
            });
            p.add(egui::Label::new(MAP_CAPTION).wrap());
        }
    }
}

pub const LINES_CAPTION: &str = "lines on one shared tick axis, frame k at the same x for every series. A lane line \
     is drawn at the UPPER bound (hi) of its included-work bound, with lo..hi as the translucent band; an infinite \
     hi has no line point and its band runs to the top (arrow cap). Every line breaks at a frame with no value \
     (unknown, refused, nothing placed, no interval): a gap is never bridged and never drawn as 0. Dots on the \
     baseline: a measured frame with nothing included on that lane. A lane line is start-attributed top-level work \
     (included spans), not occupancy and not frame time: it can exceed the PPU line.";
pub const DIST_CAPTION: &str = "distribution over every frame of the owner, measured frames only (excluded frames are \
     counted below, never binned). PPU: exact integer histogram and percentiles (ZoneStats rank rule, refused below \
     20 measured frames). Lane: per percentile the interval [p-th smallest lo, p-th smallest hi] over the frames that \
     have a bar. A bar bounds only the retained INCLUDED start-attributed top-level work of its frame (DEFINITE, not \
     extrapolated, exclusive available); AMBIGUOUS, EXTRAPOLATED and exclusive-unavailable pairs and frames with \
     nothing included are omitted, so the interval bounds the percentile of that included work only, not of all \
     the lane's work.";
pub const MAP_CAPTION: &str = "one row per series, one cell per frame: the budget verdict the bar view gives (PPU \
     exact; lanes on the exact bounds, included work only); an empty cell has no measured interval";

/// The lines mode: every series on one tick axis, slot columns shared with the bar view.
fn draw_lines(p: &mut egui::Ui, c: &ModeCtx, viewport: Rect, pal: &ui::Palette) -> ModeOut {
    let n = c.o.slots.len();
    let (rect, resp) = p.allocate_exact_size(Vec2::new(c.width, LINES_H), Sense::click());
    let painter = p.painter_at(rect);
    painter.rect_filled(rect, 2.0, pal.surface);
    let base = rect.bottom() - BAND_H - 4.0;
    let span = LINES_H - BAND_H - 12.0;
    let top_y = base - span;
    let top = c.top;
    // Pixels only: clamped, so a huge or non-finite value stays inside the graph.
    let y = |t: f64| -> f32 {
        let f = if t.is_finite() { (t / top).clamp(0.0, 1.0) } else { 1.0 };
        base - f as f32 * span
    };
    let x = |i: usize| rect.left() + (i as f32 + 0.5) * c.slot_w;
    let painted = visible_slots(n, f64::from(c.slot_w), f64::from(viewport.min.x), f64::from(viewport.max.x));
    let lr = graphs::line_range(&painted, n);
    let mut work = 0usize;
    let window = c.smoothing.window();
    let half = (c.slot_w / 2.0 - 1.0).max(0.5);
    // One series: bands (lanes), the raw line, then the smoothed line.
    let mut series = |point: &dyn Fn(usize) -> LinePoint, colour: egui::Color32, dash: graphs::Dash, width: f32| {
        let pts: Vec<(usize, LinePoint)> = lr.clone().map(|i| (i, point(i))).collect();
        work += pts.len();
        for &(i, pt) in &pts {
            let cx = x(i);
            match pt {
                LinePoint::At { y: hi, band: Some((lo, lo_open)) } => {
                    let r = Rect::from_min_max(Pos2::new(cx - half, y(hi)), Pos2::new(cx + half, y(lo).max(y(hi) + 1.0)));
                    painter.rect_filled(r, 0.0, colour.gamma_multiply(0.22));
                    if lo_open {
                        painter.circle_stroke(Pos2::new(cx, y(lo)), 2.0, Stroke::new(1.0_f32, colour));
                    }
                }
                LinePoint::Unbounded { lo, lo_open } => {
                    let r = Rect::from_min_max(Pos2::new(cx - half, top_y), Pos2::new(cx + half, y(lo).max(top_y + 1.0)));
                    painter.rect_filled(r, 0.0, colour.gamma_multiply(0.22));
                    let st = Stroke::new(1.5_f32, colour);
                    painter.line_segment([Pos2::new(cx - 3.0, top_y + 3.0), Pos2::new(cx, top_y)], st);
                    painter.line_segment([Pos2::new(cx + 3.0, top_y + 3.0), Pos2::new(cx, top_y)], st);
                    if lo_open {
                        painter.circle_stroke(Pos2::new(cx, y(lo)), 2.0, Stroke::new(1.0_f32, colour));
                    }
                }
                LinePoint::NonePlaced => {
                    painter.circle_filled(Pos2::new(cx, base + 2.0), 1.5, colour);
                }
                _ => {}
            }
        }
        for &(i, pt) in &pts {
            if pt.y().is_some_and(|v| v > top) {
                // Above the scale: drawn at the top edge with an up-arrow, never dropped.
                let cx = x(i);
                let tri = vec![Pos2::new(cx - 3.5, top_y + 5.0), Pos2::new(cx, top_y - 1.0), Pos2::new(cx + 3.5, top_y + 5.0)];
                painter.add(egui::Shape::convex_polygon(tri, colour, Stroke::NONE));
            }
        }
        let raw_colour = if window.is_some() { colour.gamma_multiply(0.35) } else { colour };
        for run in graphs::runs(pts.iter().map(|(i, pt)| (*i, pt.y()))) {
            if run.len() == 1 {
                painter.circle_filled(Pos2::new(x(run[0].0), y(run[0].1)), width + 0.5, raw_colour);
            } else {
                let line: Vec<Pos2> = run.iter().map(|&(i, v)| Pos2::new(x(i), y(v))).collect();
                polyline(&painter, line, Stroke::new(width, raw_colour), dash);
            }
        }
        if let Some(w) = window {
            let sm = graphs::smooth(lr.clone(), w, |i| point(i).y());
            work += lr.len() + w - 1;
            let smoothed = lr.clone().zip(sm).map(|(i, v)| (i, v.map(|(m, _)| m)));
            for run in graphs::runs(smoothed) {
                if run.len() == 1 {
                    painter.circle_filled(Pos2::new(x(run[0].0), y(run[0].1)), width + 0.5, colour);
                } else {
                    let line: Vec<Pos2> = run.iter().map(|&(i, v)| Pos2::new(x(i), y(v))).collect();
                    polyline(&painter, line, Stroke::new(width + 0.5, colour), dash);
                }
            }
        }
    };
    for (j, lane) in c.lanes.iter().enumerate() {
        let (colour, dash) = lane_colour(j);
        series(&|i| graphs::lane_point(c.o, lane, i), colour, dash, 1.5);
    }
    series(&|i| graphs::ppu_point(c.o, i), pal.accent, graphs::Dash::Solid, 2.0);
    // The PPU's UNKNOWN / REFUSED slots keep their glyphs on the baseline band (never a point at 0).
    for i in painted.clone() {
        let x0 = rect.left() + i as f32 * c.slot_w;
        let r = Rect::from_min_max(
            Pos2::new(x0 + 1.0, rect.bottom() - BAND_H - 2.0),
            Pos2::new(x0 + c.slot_w - 1.0, rect.bottom() - 2.0),
        );
        match graphs::ppu_point(c.o, i) {
            LinePoint::Unknown => status_mark(&painter, r, Glyph::Unknown, pal),
            LinePoint::Refused => status_mark(&painter, r, Glyph::Refused, pal),
            _ => {}
        }
        if c.selected == Some(c.o.slots[i].frame) {
            let r = Rect::from_min_max(
                Pos2::new(x0 + 1.0, rect.top() + 2.0),
                Pos2::new(x0 + c.slot_w - 1.0, rect.bottom() - 2.0),
            );
            painter.rect_stroke(r, 1.0, Stroke::new(1.5_f32, pal.text));
        }
    }
    let budget_line = c.budget_ticks.is_some();
    if let Some(t) = c.budget_ticks {
        let yb = y(t);
        painter.line_segment([Pos2::new(rect.left(), yb), Pos2::new(rect.right(), yb)], Stroke::new(1.5_f32, pal.danger));
    }
    painter.text(
        Pos2::new(rect.left() + viewport.min.x + 4.0, rect.top() + 2.0),
        Align2::LEFT_TOP,
        scale_text(c),
        FontId::proportional(10.0),
        pal.text_muted,
    );
    ModeOut { rect, clicked: resp.clicked(), at: resp.interact_pointer_pos().map(|p| p.x), painted, work, budget_line }
}

/// The value-axis caption of the lines and distribution modes.
fn scale_text(c: &ModeCtx) -> String {
    graphs::scale_caption(c.full_scale, &c.tb.ticks_f(c.top), c.fallback)
}

/// The population line of one lane's distribution: what its rows cover and what they omit.
pub fn dist_population(d: &foreign::LaneDist) -> String {
    format!(
        "population: included start-attributed top-level work on {} frames with a bar. Omitted: {} measured frames \
         with nothing included, {} frames without an interval, and every AMBIGUOUS, EXTRAPOLATED or \
         exclusive-unavailable pair (in no bar; counted in the lane footer)",
        d.bars, d.none_placed, d.no_interval
    )
}

/// One verdict-map cell in `r` (shape-coded as well as coloured).
fn map_cell(painter: &egui::Painter, r: Rect, c: &graphs::Cell, pal: &ui::Palette) {
    match c {
        graphs::Cell::Over => {
            painter.rect_filled(r, 1.0, pal.danger);
        }
        graphs::Cell::Within => {
            painter.rect_filled(r, 1.0, pal.accent);
        }
        graphs::Cell::Undetermined => {
            painter.rect_filled(r, 1.0, pal.warning);
            let st = Stroke::new(1.0_f32, pal.text);
            let mut yy = r.top() + 2.0;
            while yy < r.bottom() {
                painter.line_segment([Pos2::new(r.left(), yy), Pos2::new(r.right(), yy)], st);
                yy += 3.0;
            }
        }
        graphs::Cell::NoVerdict => {
            painter.rect_filled(r, 1.0, pal.text_muted.gamma_multiply(0.5));
        }
        graphs::Cell::Unavailable => {
            painter.rect_filled(r, 1.0, pal.text_muted.gamma_multiply(0.25));
            painter.rect_stroke(r.shrink(0.5), 1.0, Stroke::new(1.0_f32, pal.text_muted));
        }
        graphs::Cell::NonePlaced => {
            painter.circle_filled(r.center(), 1.5, pal.text_muted);
        }
        graphs::Cell::Unknown => status_mark(painter, r, Glyph::Unknown, pal),
        graphs::Cell::Refused => status_mark(painter, r, Glyph::Refused, pal),
        graphs::Cell::NoInterval => {}
    }
}

/// The verdict map: one row per series (PPU first), one cell per visible frame column.
fn draw_verdict_map(p: &mut egui::Ui, c: &ModeCtx, viewport: Rect, pal: &ui::Palette) -> ModeOut {
    let n = c.o.slots.len();
    let rows = 1 + c.lanes.len();
    let h = rows as f32 * (MAP_ROW_H + 2.0) + 4.0;
    let (rect, resp) = p.allocate_exact_size(Vec2::new(c.width, h), Sense::click());
    let painter = p.painter_at(rect);
    painter.rect_filled(rect, 2.0, pal.surface);
    let painted = visible_slots(n, f64::from(c.slot_w), f64::from(viewport.min.x), f64::from(viewport.max.x));
    let mut work = 0usize;
    for row in 0..rows {
        let y0 = rect.top() + 2.0 + row as f32 * (MAP_ROW_H + 2.0);
        for i in painted.clone() {
            let x0 = rect.left() + i as f32 * c.slot_w;
            let r = Rect::from_min_max(Pos2::new(x0 + 0.5, y0), Pos2::new(x0 + c.slot_w - 0.5, y0 + MAP_ROW_H));
            let cell = if row == 0 {
                graphs::ppu_cell(c.o, i, c.freq_hz, c.budget)
            } else {
                graphs::lane_cell(c.o, &c.lanes[row - 1], i)
            };
            work += 1;
            map_cell(&painter, r, &cell, pal);
        }
        let label = if row == 0 { "PPU" } else { c.lanes[row - 1].label.as_str() };
        let g = painter.layout_no_wrap(label.to_string(), FontId::proportional(10.0), pal.text);
        let at = Pos2::new(rect.left() + viewport.min.x + 4.0, y0 + (MAP_ROW_H - g.size().y) / 2.0);
        let bg = Rect::from_min_size(Pos2::new(at.x - 2.0, at.y), g.size() + Vec2::new(4.0, 0.0));
        painter.rect_filled(bg, 2.0, pal.surface.gamma_multiply(0.85));
        painter.galley(at, g, pal.text);
    }
    for i in painted.clone() {
        if c.selected == Some(c.o.slots[i].frame) {
            let x0 = rect.left() + i as f32 * c.slot_w;
            let r = Rect::from_min_max(Pos2::new(x0, rect.top() + 1.0), Pos2::new(x0 + c.slot_w, rect.bottom() - 1.0));
            painter.rect_stroke(r, 1.0, Stroke::new(1.5_f32, pal.text));
        }
    }
    ModeOut { rect, clicked: resp.clicked(), at: resp.interact_pointer_pos().map(|p| p.x), painted, work, budget_line: false }
}

/// The distribution mode: the PPU histogram and percentiles, then each lane's percentile intervals, on one tick
/// axis.  Returns the bins + rows drawn (fixed caps, whatever the capture size).
fn draw_distribution(p: &mut egui::Ui, c: &ModeCtx, tb: TimeBase, pal: &ui::Palette) -> usize {
    let width = p.available_width();
    let sorted = c.o.measured_sorted();
    let top = c.top;
    let bins = ((width / 8.0) as usize).clamp(8, 96);
    // Bin only the values on the axis; the rest are counted (exact) and marked at the right edge.
    let cut = sorted.partition_point(|&d| d as f64 <= top);
    let above = sorted.len() - cut;
    let hist = graphs::histogram(&sorted[..cut], bins);
    let rows_of = |j: usize| -> &[foreign::PctRow] {
        match c.dist.get(j) {
            Some(Ok(d)) => &d.rows,
            _ => &[],
        }
    };
    let lane_h = |j: usize| 16.0 + rows_of(j).len().max(1) as f32 * DIST_PCT_H;
    let h = DIST_PPU_H + 18.0 + (0..c.lanes.len()).map(lane_h).sum::<f32>() + 4.0;
    let (rect, _) = p.allocate_exact_size(Vec2::new(width, h), Sense::hover());
    let painter = p.painter_at(rect);
    painter.rect_filled(rect, 2.0, pal.surface);
    let (left, right) = (rect.left() + 40.0, rect.right() - 8.0);
    let x = |t: f64| -> f32 {
        let f = if t.is_finite() { (t / top).clamp(0.0, 1.0) } else { 1.0 };
        left + f as f32 * (right - left)
    };
    let small = |s: f32| FontId::proportional(s);
    let mut work = 0usize;
    // PPU histogram (exact counts; heights relative to the tallest bin).
    let base = rect.top() + DIST_PPU_H;
    painter.text(Pos2::new(rect.left() + 4.0, rect.top() + 2.0), Align2::LEFT_TOP, "PPU", small(10.0), pal.text);
    if let Some(hs) = &hist {
        let peak = hs.counts.iter().copied().max().unwrap_or(1).max(1);
        for (j, &k) in hs.counts.iter().enumerate() {
            work += 1;
            if k == 0 {
                continue;
            }
            let (t0, t1) = (j as f64 * hs.width as f64, (j + 1) as f64 * hs.width as f64);
            let hh = (k as f32 / peak as f32) * (DIST_PPU_H - 30.0);
            let r = Rect::from_min_max(Pos2::new(x(t0), base - hh.max(1.0)), Pos2::new(x(t1).max(x(t0) + 1.0), base));
            painter.rect_filled(r, 0.0, pal.accent);
        }
        for q in graphs::PERCENTILES {
            if let ps3dk_prof::Metric::Value(v) = c.o.percentile_of(q) {
                let xx = x(v as f64);
                let y_top = base - (DIST_PPU_H - 22.0);
                painter.line_segment([Pos2::new(xx, y_top), Pos2::new(xx, base)], Stroke::new(1.0_f32, pal.text));
                painter.text(Pos2::new(xx, y_top), Align2::CENTER_BOTTOM, format!("p{q}"), small(9.0), pal.text);
            }
        }
    }
    painter.line_segment([Pos2::new(left, base), Pos2::new(right, base)], Stroke::new(1.0_f32, pal.border_strong));
    painter.text(Pos2::new(left, base + 2.0), Align2::LEFT_TOP, "0", small(9.0), pal.text_muted);
    painter.text(Pos2::new(right, base + 2.0), Align2::RIGHT_TOP, scale_text(c), small(9.0), pal.text_muted);
    if above > 0 {
        let msg = format!("{above} more beyond the axis >");
        painter.text(Pos2::new(right, rect.top() + 2.0), Align2::RIGHT_TOP, msg, small(10.0), pal.text);
    }
    // Lanes: one bracket per percentile row, lo..hi (an infinite hi runs to the right edge with an arrow).
    let mut y0 = base + 18.0;
    for (j, l) in c.lanes.iter().enumerate() {
        let (colour, _) = lane_colour(j);
        painter.text(Pos2::new(rect.left() + 4.0, y0), Align2::LEFT_TOP, &l.label, small(10.0), pal.text);
        let mut yy = y0 + 14.0;
        if let Some(Err(_)) | None = c.dist.get(j) {
            let msg = "distribution refused (see below)";
            painter.text(Pos2::new(left, yy + DIST_PCT_H / 2.0), Align2::LEFT_CENTER, msg, small(8.0), pal.text_muted);
        }
        for row in rows_of(j) {
            work += 1;
            let cy = yy + DIST_PCT_H / 2.0;
            painter.text(Pos2::new(left - 4.0, cy), Align2::RIGHT_CENTER, row.label, small(8.0), pal.text_muted);
            match &row.value {
                Ok(b) => {
                    let (x0, x1) = (x(b.lo), b.hi.map_or(right, x));
                    let clipped = b.hi.is_none_or(|h| h > top);
                    let thin = Stroke::new(1.0_f32, colour);
                    painter.line_segment([Pos2::new(x0, cy), Pos2::new(x1.max(x0 + 1.0), cy)], Stroke::new(3.0_f32, colour));
                    painter.line_segment([Pos2::new(x0, cy - 3.0), Pos2::new(x0, cy + 3.0)], thin);
                    if !clipped {
                        painter.line_segment([Pos2::new(x1, cy - 3.0), Pos2::new(x1, cy + 3.0)], thin);
                    } else {
                        let st = Stroke::new(1.5_f32, colour);
                        painter.line_segment([Pos2::new(right - 3.0, cy - 3.0), Pos2::new(right, cy)], st);
                        painter.line_segment([Pos2::new(right - 3.0, cy + 3.0), Pos2::new(right, cy)], st);
                    }
                }
                Err(_) => {
                    painter.text(Pos2::new(left, cy), Align2::LEFT_CENTER, "refused (see below)", small(8.0), pal.text_muted);
                }
            }
            yy += DIST_PCT_H;
        }
        y0 += lane_h(j);
    }
    if let Some(t) = c.budget_ticks {
        let xb = x(t);
        let st = Stroke::new(1.5_f32, pal.danger);
        painter.line_segment([Pos2::new(xb, rect.top() + 2.0), Pos2::new(xb, rect.bottom() - 2.0)], st);
    }
    // The exact text: populations and values.
    let sm = summary(c.o, c.freq_hz, c.budget);
    p.add(
        egui::Label::new(format!(
            "PPU: {} measured frames ({} binned on the axis, {above} beyond it); {} unknown and {} refused excluded",
            sm.measured, cut, sm.unknown, sm.refused
        ))
        .wrap(),
    );
    if let Some(hs) = &hist {
        p.add(
            egui::Label::new(format!(
                "bins: {} of width {} (exact counts; total {})",
                hs.counts.len(),
                tb.ticks(hs.width),
                hs.total()
            ))
            .wrap(),
        );
    }
    let pct: Vec<String> = graphs::PERCENTILES
        .iter()
        .map(|&q| format!("p{q} {}", metric_text(&c.o.percentile_of(q), |d| tb.ticks(u128::from(*d)))))
        .collect();
    p.add(egui::Label::new(pct.join(" | ")).wrap());
    p.label(format!("max {}", sm.max.map_or("none".to_string(), |d| tb.ticks(u128::from(d)))));
    for (j, l) in c.lanes.iter().enumerate() {
        p.add_space(ui::SPACE_XS);
        p.label(egui::RichText::new(&l.label).strong());
        let d = match c.dist.get(j) {
            Some(Ok(d)) => d,
            Some(Err(why)) => {
                ui::field_message(p, ui::Tone::Warning, why);
                continue;
            }
            None => {
                ui::field_message(p, ui::Tone::Warning, "distribution not built");
                continue;
            }
        };
        p.add(egui::Label::new(dist_population(d)).wrap());
        for row in &d.rows {
            let t = match &row.value {
                Ok(b) => b.text.clone(),
                Err(why) => format!("refused ({why})"),
            };
            p.add(egui::Label::new(format!("{} {t}", row.label)).wrap());
        }
    }
    work
}
