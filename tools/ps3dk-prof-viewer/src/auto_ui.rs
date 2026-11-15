//! The Automatic page: per-frame AUTO verdicts, RSX span/busy, exclusions,
//! and steady-range summary, drawn with egui from [`crate::auto`].

use eframe::egui::{self, Color32, Pos2, Rect, RichText, Sense, Stroke, Vec2};
use ps3dk_prof::auto_rsx::{MapState, Verdict};
use ps3dk_ui as ui;

use crate::app::Page;
use crate::auto::{
    exclusions_breakdown, presentation_share, sort_frames, AutoView, FrameSortColumn, VerdictFilter,
};
use crate::timeline_ui::TimelineState;

pub const TITLE: &str = "Automatic RSX analysis";
pub const CAPTION: &str =
    "Per-frame AUTO verdicts, RSX span/busy, and exclusions from libps3dkprof automatic observation.";
pub const EMPTY_NOTICE: &str = "No automatic RSX data in this capture (build with -DPS3_PROF_AUTO=ON)";

/// State for the Automatic page surviving repaints.
#[derive(Debug, Clone, Default)]
pub struct AutoState {
    pub selected_frame: Option<u64>,
    pub steady_start: u64,
    pub steady_end: u64,
    pub steady_customized: bool,
    pub sort_col: FrameSortColumn,
    pub sort_asc: bool,
    pub filter: VerdictFilter,
}

impl AutoState {
    /// Selects a frame or clears selection.
    pub fn select(&mut self, frame: Option<u64>) {
        self.selected_frame = frame;
    }

    /// Selects a frame and positions the Timeline view over its interval.
    pub fn select_and_jump(
        &mut self,
        frame: Option<u64>,
        v: &AutoView,
        timeline: Option<&mut TimelineState>,
    ) {
        self.selected_frame = frame.filter(|k| v.report.frames.iter().any(|f| f.frame == *k));
        if let (Some(k), Some(tl)) = (self.selected_frame, timeline) {
            if let Some(f) = v.report.frames.iter().find(|f| f.frame == k) {
                tl.view.show_range(&tl.tl, f.tb_begin, f.tb_end);
                tl.fit_all = false;
            }
        }
    }

    /// Resets the page state to default bounds matching the given view.
    pub fn reset(&mut self, v: Option<&AutoView>) {
        self.selected_frame = None;
        self.steady_customized = false;
        self.sort_col = FrameSortColumn::Frame;
        self.sort_asc = true;
        self.filter = VerdictFilter::All;
        if let Some(view) = v {
            let (first, last) = view.frame_range();
            self.steady_start = first;
            self.steady_end = last;
        } else {
            self.steady_start = 0;
            self.steady_end = 0;
        }
    }

    /// Reconciles state after a live snapshot refresh (fresh = false), preserving
    /// active selection and customized steady range while clamping to valid bounds.
    pub fn refresh(&mut self, v: Option<&AutoView>) {
        let Some(view) = v else {
            self.selected_frame = None;
            self.steady_start = 0;
            self.steady_end = 0;
            return;
        };
        if view.report.frames.is_empty() {
            self.selected_frame = None;
            self.steady_start = 0;
            self.steady_end = 0;
            return;
        }
        let (first, last) = view.frame_range();
        if let Some(sel) = self.selected_frame {
            if !view.report.frames.iter().any(|f| f.frame == sel) {
                self.selected_frame = None;
            }
        }
        if self.steady_customized {
            self.steady_start = self.steady_start.clamp(first, last);
            self.steady_end = self.steady_end.clamp(first, last);
            if self.steady_start > self.steady_end {
                self.steady_start = self.steady_end;
            }
        } else {
            self.steady_start = first;
            self.steady_end = last;
        }
    }
}

/// A frame row as rendered in the frames table.
#[derive(Debug, Clone, PartialEq)]
pub struct RenderedRow {
    pub frame: u64,
    pub verdict: String,
    pub interval_text: String,
    pub span_text: String,
    pub busy_text: String,
    pub reason: String,
}

/// What the page drew on the last repaint (used in headless tests).
#[derive(Debug, Clone, Default)]
pub struct Drawn {
    pub empty: bool,
    pub empty_message: String,
    pub map_state: String,
    pub is_final: bool,
    pub ops_dropped: u64,
    pub frame_marks_missed: bool,
    pub coverage_rows_lost: u64,
    pub verdict_strip_cells: usize,
    pub steady_frame_count: usize,
    pub steady_mix: Vec<(Verdict, usize)>,
    pub steady_median_interval_ms: Option<f64>,
    pub steady_median_span_ppu_ticks: Option<(u64, u64)>,
    pub steady_median_busy_ppu_ticks: Option<(u64, u64)>,
    pub table_rows: usize,
    pub rendered_frame_rows: Vec<RenderedRow>,
    pub selected_frame: Option<u64>,
    pub jumped_timeline: Option<(u64, u64)>,
}

/// Returns the theme color for a verdict.
pub fn verdict_color(v: Verdict, pal: &ui::Palette) -> Color32 {
    match v {
        Verdict::RsxBound => pal.danger,
        Verdict::PpuBound => pal.accent,
        Verdict::Pacing => pal.success,
        Verdict::Mixed => pal.warning,
        Verdict::Unknown => pal.text_muted,
    }
}

/// Returns the Tone for a verdict badge.
pub fn verdict_tone(v: Verdict) -> ui::Tone {
    match v {
        Verdict::RsxBound => ui::Tone::Danger,
        Verdict::PpuBound => ui::Tone::Accent,
        Verdict::Pacing => ui::Tone::Success,
        Verdict::Mixed => ui::Tone::Warning,
        Verdict::Unknown => ui::Tone::Neutral,
    }
}

/// Pill-shaped badge styled like ps3dk_ui::badge, but with hover-only sense and no internal wrapping.
fn auto_badge_with_sense(ui: &mut egui::Ui, text: &str, tone: ui::Tone, sense: Sense) -> egui::Response {
    let p = ui::Palette::of(ui);
    let (fill, outline, color) = match tone {
        ui::Tone::Neutral => (p.surface_alt, p.border, p.text_muted),
        ui::Tone::Accent => (p.accent.gamma_multiply(0.18), p.accent.gamma_multiply(0.55), p.accent),
        tone => {
            let color = tone.color(&p);
            let fill = color.gamma_multiply(0.18);
            (fill, color.gamma_multiply(0.45), color)
        }
    };
    let font_id = egui::FontId::proportional(11.5);
    let galley = ui.painter().layout_no_wrap(text.to_string(), font_id, color);
    let text_size = galley.size();
    let pad_x = 7.0;
    let pad_y = 1.0;
    let total_size = egui::vec2(text_size.x + pad_x * 2.0, (text_size.y + pad_y * 2.0).max(18.0));
    let (rect, response) = ui.allocate_exact_size(total_size, sense);
    if ui.is_rect_visible(rect) {
        ui.painter().rect(rect, egui::Rounding::same(9.0), fill, egui::Stroke::new(1.0, outline));
        let text_pos = egui::pos2(rect.min.x + pad_x, rect.min.y + (rect.height() - text_size.y) * 0.5);
        ui.painter().galley(text_pos, galley, color);
    }
    response
}

fn auto_badge(ui: &mut egui::Ui, text: &str, tone: ui::Tone) -> egui::Response {
    auto_badge_with_sense(ui, text, tone, Sense::hover())
}

fn auto_badge_clickable(ui: &mut egui::Ui, text: &str, tone: ui::Tone) -> egui::Response {
    auto_badge_with_sense(ui, text, tone, Sense::click())
}

// Automatic-specific wrapping. Existing shared widgets retain their contract.
fn auto_card_header(ui: &mut egui::Ui, title: &str, subtitle: Option<&str>, trailing: impl FnOnce(&mut egui::Ui)) {
    let p = ui::Palette::of(ui);
    ui.horizontal(|h| {
        h.label(RichText::new(title).text_style(ui::title_style()).strong().color(p.text));
        h.with_layout(egui::Layout::right_to_left(egui::Align::Min), trailing);
    });
    if let Some(text) = subtitle {
        ui.add_space(2.0);
        ui.add(egui::Label::new(RichText::new(text).small().color(p.text_muted)).wrap());
    }
    ui.add_space(ui::SPACE_SM);
}

fn bounds_ms((lo, hi): (f64, f64)) -> String {
    if lo == hi { format!("{lo:.3} ms") } else { format!("{lo:.3}..{hi:.3} ms") }
}

/// Draws the Automatic page. Returns Some(Page) if an interaction requested a page change.
pub fn page(
    p: &mut egui::Ui,
    v: &AutoView,
    st: &mut AutoState,
    mut timeline: Option<&mut TimelineState>,
    drawn: &mut Drawn,
) -> Option<Page> {
    *drawn = Drawn::default();
    let pal = ui::Palette::of(p);

    ui::page_header(p, TITLE, CAPTION);

    if !v.has_auto() {
        drawn.empty = true;
        drawn.empty_message = EMPTY_NOTICE.to_string();
        ui::empty_state(p, EMPTY_NOTICE, "This capture does not contain automatic profiler data.");
        return None;
    }

    st.refresh(Some(v));
    drawn.selected_frame = st.selected_frame;

    // ── 1. CAPTURE FACTS ───────────────────────────────────────────────
    draw_capture_facts(p, v, drawn, &pal);
    p.add_space(ui::SPACE_MD);

    // ── 2. VERDICT STRIP ───────────────────────────────────────────────
    draw_verdict_strip(p, v, st, timeline.as_deref_mut(), drawn, &pal);
    p.add_space(ui::SPACE_MD);

    // ── 3. STEADY RANGE SUMMARY ────────────────────────────────────────
    draw_steady_range_summary(p, v, st, drawn, &pal);
    p.add_space(ui::SPACE_MD);

    // ── 4. FRAMES TABLE ────────────────────────────────────────────────
    let next_page = draw_frames_table(p, v, st, timeline, drawn, &pal);

    next_page
}

/// Returns the actual visible available width inside a card, constrained by the window edge.
fn card_available_width(ui: &egui::Ui) -> f32 {
    let right_limit = ui.clip_rect().right() - (ui::SPACE_XL + 4.0);
    let card_right_pad = ui::SPACE_LG + 2.0;
    let visible_w = (right_limit - ui.cursor().left() - card_right_pad - 4.0).max(60.0);
    (ui.available_width() - card_right_pad - 4.0).max(60.0).min(visible_w)
}

fn fact_row<F>(ui: &mut egui::Ui, label: &str, add_value: F)
where
    F: FnOnce(&mut egui::Ui),
{
    ui.horizontal_top(|row| {
        row.allocate_ui_with_layout(
            egui::vec2(180.0, 0.0),
            egui::Layout::top_down(egui::Align::Min),
            |l| {
                l.set_max_width(180.0);
                l.label(RichText::new(label).strong());
            },
        );
        row.add_space(ui::SPACE_MD);
        row.vertical(|v| {
            add_value(v);
        });
    });
    ui.add_space(ui::SPACE_SM);
}

fn draw_capture_facts(p: &mut egui::Ui, v: &AutoView, drawn: &mut Drawn, _pal: &ui::Palette) {
    ui::card(p, |c| {
        let card_w = card_available_width(c);
        c.set_max_width(card_w);
        auto_card_header(
            c,
            "CAPTURE FACTS",
            Some("Clock map qualification, capture completion and operational counters."),
            |_| {},
        );
        c.add_space(ui::SPACE_XS);

        // Clock map state
        fact_row(c, "Clock map", |v_ui| {
            v_ui.horizontal_top(|h| {
                match &v.report.map {
                    Some(MapState::Qualified(m)) => {
                        let text = format!(
                            "QUALIFIED · {} anchors, PPU/RSX slope [{}/{}, {}/{}], decisive hold-outs odd {} even {}",
                            m.anchors,
                            m.slope_lo.num,
                            m.slope_lo.den,
                            m.slope_hi.num,
                            m.slope_hi.den,
                            m.decisive.0,
                            m.decisive.1
                        );
                        drawn.map_state = "QUALIFIED".to_string();
                        auto_badge(h, "QUALIFIED", ui::Tone::Success);
                        h.add_space(ui::SPACE_XS);
                        h.add(egui::Label::new(&text).wrap()).on_hover_text(&text);
                    }
                    Some(MapState::Unqualified(why)) => {
                        drawn.map_state = format!("UNQUALIFIED ({why})");
                        auto_badge(h, "UNQUALIFIED", ui::Tone::Warning);
                        h.add_space(ui::SPACE_XS);
                        let text = format!("AUTO_CLOCK_UNQUALIFIED: {why} (raw RSX ticks only)");
                        h.add(egui::Label::new(&text).wrap()).on_hover_text(&text);
                    }
                    None => {
                        drawn.map_state = "NO MAP".to_string();
                        auto_badge(h, "NO MAP", ui::Tone::Neutral);
                        h.add_space(ui::SPACE_XS);
                        let text = "No clock anchors recorded";
                        h.add(egui::Label::new(text).wrap()).on_hover_text(text);
                    }
                }
            });
        });

        // Final vs Provisional
        drawn.is_final = v.is_final();
        fact_row(c, "Capture state", |v_ui| {
            v_ui.horizontal_top(|h| {
                if v.is_final() {
                    auto_badge(h, "FINAL", ui::Tone::Success);
                    h.add_space(ui::SPACE_XS);
                    let text = "Closed with complete stop / finalization";
                    h.add(egui::Label::new(text).wrap()).on_hover_text(text);
                } else {
                    auto_badge(h, "PROVISIONAL", ui::Tone::Warning);
                    h.add_space(ui::SPACE_XS);
                    let text = "Provisional snapshot / open session";
                    h.add(egui::Label::new(text).wrap()).on_hover_text(text);
                }
            });
        });

        // Ops dropped
        let dropped = v.ops_dropped();
        drawn.ops_dropped = dropped;
        fact_row(c, "Ops dropped past ceiling", |v_ui| {
            v_ui.horizontal_top(|h| {
                if dropped > 0 {
                    auto_badge(h, &format!("{dropped} DROPPED"), ui::Tone::Warning);
                    h.add_space(ui::SPACE_XS);
                    let text = "Operations parsed but not retained past operation ceiling";
                    h.add(egui::Label::new(text).wrap()).on_hover_text(text);
                } else {
                    auto_badge(h, "0 DROPPED", ui::Tone::Success);
                    h.add_space(ui::SPACE_XS);
                    let text = "All operations retained within ceiling";
                    h.add(egui::Label::new(text).wrap()).on_hover_text(text);
                }
            });
        });

        // Marker refusal and lost coverage rows are independent wire facts.
        drawn.frame_marks_missed = v.frame_marks_missed();
        fact_row(c, "RSX frame marks", |v_ui| {
            v_ui.horizontal_top(|h| {
                if drawn.frame_marks_missed {
                    auto_badge(h, "MISSED", ui::Tone::Danger);
                    h.add(egui::Label::new("At least one RSX frame marker was refused; no count is recorded.").wrap());
                } else {
                    auto_badge(h, "NO MISSES REPORTED", ui::Tone::Neutral);
                }
            });
        });
        let lost = v.coverage_rows_lost();
        drawn.coverage_rows_lost = lost;
        fact_row(c, "Coverage rows lost", |v_ui| {
            auto_badge(v_ui, &format!("{lost} LOST"), if lost == 0 { ui::Tone::Neutral } else { ui::Tone::Danger });
        });
    });
}

fn draw_verdict_strip(
    p: &mut egui::Ui,
    v: &AutoView,
    st: &mut AutoState,
    mut timeline: Option<&mut TimelineState>,
    drawn: &mut Drawn,
    pal: &ui::Palette,
) {
    ui::card(p, |c| {
        let card_w = card_available_width(c);
        c.set_max_width(card_w);
        auto_card_header(
            c,
            "VERDICT STRIP",
            Some("Per-frame verdict sequence; click a frame to inspect its details and jump the Timeline."),
            |_| {},
        );
        c.add_space(ui::SPACE_XS);

        // Counts row
        c.allocate_ui_with_layout(
            egui::vec2(card_w, 0.0),
            egui::Layout::left_to_right(egui::Align::Center).with_main_wrap(true),
            |h| {
                h.set_max_width(card_w);
                h.label(RichText::new(format!("Total: {}", v.report.frames.len())).strong());
                h.add_space(ui::SPACE_SM);
                for (verdict, count) in v.verdict_counts() {
                    let badge_resp = auto_badge_clickable(h, &format!("{}: {}", verdict.key(), count), verdict_tone(verdict));
                    if badge_resp.clicked() {
                        st.filter = match verdict {
                            Verdict::RsxBound => VerdictFilter::RsxBound,
                            Verdict::PpuBound => VerdictFilter::PpuBound,
                            Verdict::Pacing => VerdictFilter::Pacing,
                            Verdict::Mixed => VerdictFilter::Mixed,
                            Verdict::Unknown => VerdictFilter::Unknown,
                        };
                    }
                    h.add_space(ui::SPACE_XS);
                }
            },
        );
        c.add_space(ui::SPACE_SM);

        // Strip of frame cells scaled and wrapped to fit panel width
        let frame_count = v.report.frames.len();
        drawn.verdict_strip_cells = frame_count;
        if frame_count == 0 {
            c.label("No frames in this capture.");
            return;
        }

        let avail_w = (card_available_width(c) - 4.0).max(60.0);
        let min_cell_w = 6.0f32;
        let max_cell_w = 20.0f32;
        let pref_cell_w = 14.0f32;
        let cell_h = 24.0f32;
        let gap = 2.0f32;

        // Can all frames fit on 1 row with at least min_cell_w?
        let single_row_w = (avail_w - (frame_count.saturating_sub(1) as f32 * gap)) / frame_count as f32;
        let (cols_per_row, cell_w) = if single_row_w >= min_cell_w {
            (frame_count, single_row_w.min(max_cell_w))
        } else {
            let cols = ((avail_w + gap) / (pref_cell_w + gap)).floor().max(1.0) as usize;
            let actual_w = ((avail_w - (cols.saturating_sub(1) as f32 * gap)) / cols as f32).clamp(min_cell_w, max_cell_w);
            (cols, actual_w)
        };

        let num_rows = (frame_count + cols_per_row - 1) / cols_per_row;
        let total_h = num_rows as f32 * cell_h + num_rows.saturating_sub(1) as f32 * gap;
        let strip_w = (cols_per_row as f32 * cell_w + cols_per_row.saturating_sub(1) as f32 * gap).min(avail_w);

        let (rect, _) = c.allocate_exact_size(Vec2::new(strip_w, total_h), Sense::hover());
        let painter = c.painter_at(rect);

        for (idx, f) in v.report.frames.iter().enumerate() {
            let row = idx / cols_per_row;
            let col = idx % cols_per_row;
            let x0 = rect.left() + col as f32 * (cell_w + gap);
            let y0 = rect.top() + row as f32 * (cell_h + gap);
            let cell_rect = Rect::from_min_size(Pos2::new(x0, y0), Vec2::new(cell_w, cell_h));
            let color = verdict_color(f.verdict, pal);
            let is_sel = st.selected_frame == Some(f.frame);

            painter.rect_filled(cell_rect, 2.0, color);

            if is_sel {
                painter.rect_stroke(cell_rect.expand(1.0), 3.0, Stroke::new(2.0_f32, pal.text));
            }

            // Interactive response using interact so it doesn't advance cursor
            let resp = c.interact(cell_rect, c.id().with(("auto_strip_cell", idx)), Sense::click());
            let interval_ms = v.frame_interval_ms(f);
            let interval_ticks = AutoView::frame_interval_ticks(f);

            resp.clone().on_hover_ui(|tip| {
                tip.label(RichText::new(format!("Frame #{} (owner {})", f.frame, f.owner)).strong());
                auto_badge(tip, f.verdict.key(), verdict_tone(f.verdict));
                tip.label(RichText::new(&f.reason).italics());
                tip.add_space(ui::SPACE_XS);
                tip.label(format!("Interval: {:.3} ms ({} PPU ticks)", interval_ms, interval_ticks));
                if let Some((lo, hi)) = f.rsx_span_ppu {
                    tip.label(format!("RSX Span: {:.3}..{:.3} ms ({} raw ticks)", v.ticks_to_ms(lo), v.ticks_to_ms(hi), f.rsx_span_raw.unwrap_or(0)));
                } else if let Some(raw) = f.rsx_span_raw {
                    tip.label(format!("RSX Span: {raw} raw ticks (unmapped)"));
                }
                if let Some((lo, hi)) = f.rsx_busy_ppu {
                    tip.label(format!("RSX Busy: {:.3}..{:.3} ms ({} raw ticks)", v.ticks_to_ms(lo), v.ticks_to_ms(hi), f.rsx_busy_raw.unwrap_or(0)));
                } else if let Some(raw) = f.rsx_busy_raw {
                    tip.label(format!("RSX Busy: {raw} raw ticks (unmapped)"));
                }
                let ex_count: u64 = f.excluded.iter().sum();
                if ex_count > 0 {
                    tip.label(format!("Excluded: {} draws", ex_count));
                }
            });

            if resp.clicked() {
                st.select_and_jump(Some(f.frame), v, timeline.as_deref_mut());
                drawn.selected_frame = Some(f.frame);
                drawn.jumped_timeline = Some((f.tb_begin, f.tb_end));
            }
        }
    });
}

fn draw_steady_range_summary(p: &mut egui::Ui, v: &AutoView, st: &mut AutoState, drawn: &mut Drawn, _pal: &ui::Palette) {
    ui::card(p, |c| {
        let card_w = card_available_width(c);
        c.set_max_width(card_w);

        auto_card_header(
            c,
            "STEADY RANGE SUMMARY",
            Some("Interactive frame range selector; medians and verdict mix across steady frames."),
            |_| {},
        );
        c.add_space(ui::SPACE_XS);

        let (min_frame, max_frame) = v.frame_range();

        // Range input controls
        c.allocate_ui_with_layout(
            egui::vec2(card_w, 0.0),
            egui::Layout::left_to_right(egui::Align::Center).with_main_wrap(true),
            |h| {
                h.set_max_width(card_w);
                h.label("Range:");
                let start_resp = h.add(egui::DragValue::new(&mut st.steady_start).range(min_frame..=st.steady_end).prefix("Frame "));
                h.label("to");
                let end_resp = h.add(egui::DragValue::new(&mut st.steady_end).range(st.steady_start..=max_frame).prefix("Frame "));

                if start_resp.changed() || end_resp.changed() {
                    st.steady_customized = true;
                }

                if ui::quiet_button(h, "Reset to all").clicked() {
                    st.steady_start = min_frame;
                    st.steady_end = max_frame;
                    st.steady_customized = false;
                }
            },
        );
        c.add_space(ui::SPACE_SM);

        let summary = v.steady_summary(st.steady_start, st.steady_end);
        drawn.steady_frame_count = summary.frame_count;
        drawn.steady_mix = summary.verdict_counts.to_vec();
        drawn.steady_median_interval_ms = summary.median_interval_ms();
        drawn.steady_median_span_ppu_ticks = summary.median_span_ppu_ticks;
        drawn.steady_median_busy_ppu_ticks = summary.median_busy_ppu_ticks;

        if summary.frame_count == 0 {
            c.label("No frames in selected range.");
            return;
        }

        // Verdict mix
        fact_row(c, "Verdict mix", |v_ui| {
            v_ui.horizontal_wrapped(|h| {
                h.label(format!("{} frames: ", summary.frame_count));
                for (verdict, count) in &summary.verdict_counts {
                    let pct = summary.mix_pct(*verdict);
                    auto_badge(h, &format!("{}: {} ({:.1}%)", verdict.key(), count, pct), verdict_tone(*verdict));
                    h.add_space(ui::SPACE_XS);
                }
            });
        });

        // Median Frame Interval
        fact_row(c, "Median Frame Interval", |v_ui| {
            v_ui.horizontal_top(|h| {
                let interval_text = summary
                    .median_interval_ms()
                    .map_or("none".into(), |ms| format!("{ms:.3} ms"));
                let interval_ticks_tip = summary
                    .median_interval_ticks
                    .map_or(String::new(), |t| format!("{t} PPU ticks"));
                h.label(&interval_text).on_hover_text(interval_ticks_tip);
            });
        });

        // Median RSX Span
        fact_row(c, "Median RSX Span", |v_ui| {
            v_ui.horizontal_top(|h| {
                let span_text = if let Some(ms) = summary.median_span_ms() {
                    bounds_ms(ms)
                } else if let Some(raw) = summary.median_span_raw_ticks {
                    format!("{raw} raw ticks (unmapped)")
                } else {
                    "-".into()
                };
                let span_tip = format!(
                    "PPU ticks: {}, raw ticks: {}",
                    summary.median_span_ppu_ticks.map_or("-".into(), |(lo, hi)| format!("{lo}..{hi}")),
                    summary.median_span_raw_ticks.map_or("-".into(), |t| t.to_string())
                );
                h.label(&span_text).on_hover_text(span_tip);
            });
        });

        // Median RSX Busy
        fact_row(c, "Median RSX Busy", |v_ui| {
            v_ui.horizontal_top(|h| {
                let busy_text = if let Some(ms) = summary.median_busy_ms() {
                    bounds_ms(ms)
                } else if let Some(raw) = summary.median_busy_raw_ticks {
                    format!("{raw} raw ticks (unmapped)")
                } else {
                    "-".into()
                };
                let busy_tip = format!(
                    "PPU ticks: {}, raw ticks: {}",
                    summary.median_busy_ppu_ticks.map_or("-".into(), |(lo, hi)| format!("{lo}..{hi}")),
                    summary.median_busy_raw_ticks.map_or("-".into(), |t| t.to_string())
                );
                h.label(&busy_text).on_hover_text(busy_tip);
            });
        });
    });
}

fn draw_frames_table(
    p: &mut egui::Ui,
    v: &AutoView,
    st: &mut AutoState,
    mut timeline: Option<&mut TimelineState>,
    drawn: &mut Drawn,
    pal: &ui::Palette,
) -> Option<Page> {
    let mut next_page = None;
    ui::card(p, |c| {
        let card_w = card_available_width(c);
        c.set_max_width(card_w);
        auto_card_header(
            c,
            "FRAMES",
            Some("Per-frame details, intervals, RSX metrics, exclusions and reason text."),
            |_| {},
        );
        c.add_space(ui::SPACE_XS);

        // Filter buttons
        c.allocate_ui_with_layout(
            egui::vec2(card_w, 0.0),
            egui::Layout::left_to_right(egui::Align::Center).with_main_wrap(true),
            |h| {
                h.set_max_width(card_w);
                h.label("Filter by verdict:");
                for filter_option in [
                    VerdictFilter::All,
                    VerdictFilter::RsxBound,
                    VerdictFilter::PpuBound,
                    VerdictFilter::Pacing,
                    VerdictFilter::Mixed,
                    VerdictFilter::Unknown,
                ] {
                    let is_sel = st.filter == filter_option;
                    if h.selectable_label(is_sel, filter_option.label()).clicked() {
                        st.filter = filter_option;
                    }
                }
            },
        );
        c.add_space(ui::SPACE_SM);

        // Filter and sort frame rows
        let mut visible_frames: Vec<&ps3dk_prof::auto_rsx::FrameView> = v
            .report
            .frames
            .iter()
            .filter(|f| st.filter.matches(f.verdict))
            .collect();
        sort_frames(&mut visible_frames, st.sort_col, st.sort_asc);
        drawn.table_rows = visible_frames.len();

        let show_call_covered = v.report.call_bracket || visible_frames.iter().any(|f| f.call_covered > 0);

        // Table headers with sort toggling
        let num_cols = if show_call_covered { 9 } else { 8 };
        let table_w = card_available_width(c);
        let reason_col_w = (table_w - 550.0).clamp(180.0, 360.0);

        c.allocate_ui_with_layout(
            egui::vec2(table_w, 0.0),
            egui::Layout::top_down(egui::Align::Min),
            |t_box| {
                t_box.set_max_width(table_w);
                egui::ScrollArea::horizontal()
                    .id_salt("auto_table_scroll")
                    .max_width(table_w)
                    .show(t_box, |t_ui| {
                egui::Grid::new("auto_frames_table_grid")
                    .num_columns(num_cols)
                    .striped(true)
                    .spacing([ui::SPACE_MD, ui::SPACE_XS])
                    .show(t_ui, |g| {
                        // Header row
                        let mut header_btn = |g: &mut egui::Ui, name: &str, col: FrameSortColumn| {
                            let is_active = st.sort_col == col;
                            let arrow = if is_active {
                                if st.sort_asc { " ▲" } else { " ▼" }
                            } else {
                                ""
                            };
                            let resp = g.button(RichText::new(format!("{name}{arrow}")).strong());
                            if resp.clicked() {
                                if is_active {
                                    st.sort_asc = !st.sort_asc;
                                } else {
                                    st.sort_col = col;
                                    st.sort_asc = true;
                                }
                            }
                        };

                        header_btn(g, "Frame", FrameSortColumn::Frame);
                        header_btn(g, "Verdict", FrameSortColumn::Verdict);
                        header_btn(g, "Interval (ms)", FrameSortColumn::Interval);
                        header_btn(g, "RSX Span (ms)", FrameSortColumn::Span);
                        header_btn(g, "RSX Busy (ms)", FrameSortColumn::Busy);
                        g.label(RichText::new("Flip-Wait / Polls").strong());
                        header_btn(g, "Excluded", FrameSortColumn::Excluded);
                        if show_call_covered {
                            header_btn(g, "Call Covered", FrameSortColumn::CallCovered);
                        }
                        g.add_sized([reason_col_w, 18.0], egui::Label::new(RichText::new("Reason").strong()));
                        g.end_row();

                        // Rows
                        for f in &visible_frames {
                            let is_sel = st.selected_frame == Some(f.frame);
                            let f_resp = g.selectable_label(is_sel, format!("#{}", f.frame));
                            if f_resp.clicked() {
                                st.select_and_jump(Some(f.frame), v, timeline.as_deref_mut());
                                drawn.selected_frame = Some(f.frame);
                                drawn.jumped_timeline = Some((f.tb_begin, f.tb_end));
                            }

                            // Verdict
                            auto_badge(g, f.verdict.key(), verdict_tone(f.verdict));

                            // Interval
                            let interval_ms = v.frame_interval_ms(f);
                            let interval_ticks = AutoView::frame_interval_ticks(f);
                            g.label(format!("{interval_ms:.3} ms"))
                                .on_hover_text(format!("{interval_ticks} PPU ticks"));

                            // RSX Span
                            if let Some((lo, hi)) = f.rsx_span_ppu {
                                let lo_ms = v.ticks_to_ms(lo);
                                let hi_ms = v.ticks_to_ms(hi);
                                let span_text = if lo == hi {
                                    format!("{lo_ms:.3} ms")
                                } else {
                                    format!("{lo_ms:.3}..{hi_ms:.3} ms")
                                };
                                let raw = f.rsx_span_raw.unwrap_or(0);
                                g.label(span_text)
                                    .on_hover_text(format!("Raw: {raw} RSX ticks | PPU: {lo}..{hi} ticks"));
                            } else if let Some(raw) = f.rsx_span_raw {
                                g.label(format!("{raw} raw"))
                                    .on_hover_text("Unmapped clock: raw RSX ticks only");
                            } else {
                                g.label("-");
                            }

                            // RSX Busy
                            if let Some((lo, hi)) = f.rsx_busy_ppu {
                                let lo_ms = v.ticks_to_ms(lo);
                                let hi_ms = v.ticks_to_ms(hi);
                                let busy_text = if lo == hi {
                                    format!("{lo_ms:.3} ms")
                                } else {
                                    format!("{lo_ms:.3}..{hi_ms:.3} ms")
                                };
                                let raw = f.rsx_busy_raw.unwrap_or(0);
                                g.label(busy_text)
                                    .on_hover_text(format!("Raw: {raw} RSX ticks | PPU: {lo}..{hi} ticks"));
                            } else if let Some(raw) = f.rsx_busy_raw {
                                g.label(format!("{raw} raw"))
                                    .on_hover_text("Unmapped clock: raw RSX ticks only");
                            } else {
                                g.label("-");
                            }

                            // Flip-Wait / Poll shares
                            if let Some(share) = presentation_share(f) {
                                let mut parts = Vec::new();
                                if let Some(pct) = share.flip_wait_pct {
                                    parts.push(format!("flip-wait: {pct:.1}%"));
                                }
                                if let Some(pct) = share.cadence_pct {
                                    parts.push(format!("polling: {pct:.1}%"));
                                }
                                if parts.is_empty() {
                                    if let Some(cnt) = share.legacy_polls_count {
                                        parts.push(format!("{cnt} polls"));
                                    }
                                }
                                let summary_str = if parts.is_empty() { "-".into() } else { parts.join(", ") };
                                let tooltip = format!(
                                    "Flip-wait: {} ticks, {} scopes | Polling: {} ticks, {} runs",
                                    share.flip_wait_ticks.map_or("-".into(), |t| t.to_string()),
                                    share.flip_wait_scopes.map_or("-".into(), |s| s.to_string()),
                                    share.cadence_ticks.map_or("-".into(), |t| t.to_string()),
                                    share.cadence_runs.map_or("-".into(), |r| r.to_string())
                                );
                                g.label(summary_str).on_hover_text(tooltip);
                            } else {
                                g.label("-");
                            }

                            // Excluded
                            let ex_total: u64 = f.excluded.iter().sum();
                            if ex_total > 0 {
                                let breakdown = exclusions_breakdown(f);
                                let tip_lines: Vec<String> = breakdown
                                    .iter()
                                    .map(|(k, r, c)| format!("{k}: {c} ({r})"))
                                    .collect();
                                g.label(RichText::new(format!("{ex_total}")).color(pal.warning))
                                    .on_hover_text(tip_lines.join("\n"));
                            } else {
                                g.label("0");
                            }

                            // Call Covered
                            if show_call_covered {
                                if f.call_covered > 0 {
                                    g.label(format!("{}", f.call_covered));
                                } else {
                                    g.label("-");
                                }
                            }

                            // Reason
                            g.add_sized([reason_col_w, 18.0], egui::Label::new(&f.reason).truncate())
                                .on_hover_text(&f.reason);

                            g.end_row();
                        }
                    });
            });
        });

        // ── Selected Frame Detail Panel ─────────────────────────────────────
        if let Some(sel_frame_id) = st.selected_frame {
            if let Some(sel_f) = v.report.frames.iter().find(|f| f.frame == sel_frame_id) {
                c.add_space(ui::SPACE_MD);
                next_page = draw_frame_detail(c, v, sel_f, timeline, pal);
            }
        }
    });
    next_page
}

fn draw_frame_detail(
    p: &mut egui::Ui,
    v: &AutoView,
    f: &ps3dk_prof::auto_rsx::FrameView,
    mut timeline: Option<&mut TimelineState>,
    pal: &ui::Palette,
) -> Option<Page> {
    let mut next_page = None;
    ui::card(p, |d| {
        let card_w = card_available_width(d);
        d.set_max_width(card_w);
        let avail_w = d.available_width().max(60.0);
        let col2_max_w = (avail_w - 200.0).max(120.0);

        d.allocate_ui_with_layout(
            egui::vec2(card_w, 0.0),
            egui::Layout::left_to_right(egui::Align::Center).with_main_wrap(true),
            |h| {
                h.set_max_width(card_w);
                h.label(RichText::new(format!("FRAME #{} DETAIL", f.frame)).strong());
                auto_badge(h, f.verdict.key(), verdict_tone(f.verdict));
                h.add_space(ui::SPACE_LG);

                if ui::primary_button(h, "View in Timeline").clicked() {
                    if let Some(tl) = timeline.as_deref_mut() {
                        tl.view.show_range(&tl.tl, f.tb_begin, f.tb_end);
                        tl.fit_all = false;
                    }
                    next_page = Some(Page::Timeline);
                }
            },
        );
        d.add_space(ui::SPACE_XS);

        d.add(egui::Label::new(RichText::new(&f.reason).italics()).wrap()).on_hover_text(&f.reason);
        d.add_space(ui::SPACE_XS);

        egui::Grid::new("auto_frame_detail_grid")
            .num_columns(2)
            .spacing([ui::SPACE_LG, ui::SPACE_XS])
            .show(d, |g| {
                g.label("Owner:");
                g.label(format!("Thread {}", f.owner));
                g.end_row();

                g.label("PPU Timebase:");
                let tb_text = format!(
                    "begin: {} · end: {} ({} ticks, {:.3} ms)",
                    f.tb_begin,
                    f.tb_end,
                    f.tb_end.saturating_sub(f.tb_begin),
                    v.frame_interval_ms(f)
                );
                g.horizontal_wrapped(|h| {
                    h.set_max_width(col2_max_w);
                    h.add(egui::Label::new(&tb_text).wrap()).on_hover_text(&tb_text);
                });
                g.end_row();

                g.label("RSX Frame Marks:");
                let raw_marks = format!(
                    "begin: {} · end: {}",
                    f.rsx_frame_raw.0.map_or("none".into(), |x| x.to_string()),
                    f.rsx_frame_raw.1.map_or("none".into(), |x| x.to_string())
                );
                let mapped_marks = f.rsx_frame_ppu.map_or(String::new(), |(a, b)| {
                    format!(" · PPU: {a}..{b} ticks ({:.3}..{:.3} ms)", v.ticks_to_ms(a), v.ticks_to_ms(b))
                });
                let marks_text = format!("{raw_marks}{mapped_marks}");
                g.horizontal_wrapped(|h| {
                    h.set_max_width(col2_max_w);
                    h.add(egui::Label::new(&marks_text).wrap()).on_hover_text(&marks_text);
                });
                g.end_row();

                if let Some(fw) = f.fifo_wait {
                    g.label("FIFO Wait Evidence:");
                    g.label(format!(">= {fw} PPU ticks ({:.3} ms)", v.ticks_to_ms(fw)));
                    g.end_row();
                }

                if f.call_brackets != 0 || f.call_covered != 0 {
                    g.label("CALL Brackets:");
                    g.label(format!("{} brackets · {} draws covered", f.call_brackets, f.call_covered));
                    g.end_row();
                }
            });

        // Exclusions breakdown table
        let breakdown = exclusions_breakdown(f);
        if !breakdown.is_empty() {
            d.add_space(ui::SPACE_XS);
            ui::section_label(d, "EXCLUDED DRAWS");
            let ex_w = card_available_width(d);
            d.allocate_ui_with_layout(
                egui::vec2(ex_w, 0.0),
                egui::Layout::top_down(egui::Align::Min),
                |ed_box| {
                    ed_box.set_max_width(ex_w);
                    egui::ScrollArea::horizontal()
                        .id_salt("auto_frame_exclusions_scroll")
                        .max_width(ex_w)
                        .show(ed_box, |ed_ui| {
                            egui::Grid::new("auto_frame_exclusions_grid")
                                .num_columns(3)
                                .striped(true)
                                .spacing([ui::SPACE_LG, ui::SPACE_XS])
                                .show(ed_ui, |g| {
                                    g.label(RichText::new("Kind").strong());
                                    g.label(RichText::new("Reason").strong());
                                    g.label(RichText::new("Excluded Count").strong());
                                    g.end_row();

                                    for (kind, reason, count) in breakdown {
                                        g.label(kind);
                                        g.label(reason);
                                        g.label(RichText::new(format!("{count}")).color(pal.warning));
                                        g.end_row();
                                    }
                                });
                        });
                },
            );
        }
    });
    next_page
}
