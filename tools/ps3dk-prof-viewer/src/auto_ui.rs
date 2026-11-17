//! The Automatic page: per-frame AUTO verdicts, RSX span/busy, exclusions,
//! and steady-range summary, drawn with egui from [`crate::auto`].

use eframe::egui::{self, Color32, Pos2, Rect, RichText, Sense, Stroke, Vec2};
use ps3dk_prof::auto_rsx::{MapState, Verdict, reject_reason};
use ps3dk_ui as ui;

use crate::app::Page;
use crate::auto::{
    exclusions_breakdown, presentation_share, ring_exclusions_breakdown, sort_frames, AutoView,
    FrameSortColumn, VerdictFilter,
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
    pub excluded_text: String,
    pub exclusions: Vec<(&'static str, &'static str, u64)>,
    pub call_covered_text: Option<String>,
    pub presentation_text: String,
}

/// What the page drew on the last repaint (used in headless tests).
#[derive(Debug, Clone, Default)]
pub struct Drawn {
    pub empty: bool,
    pub empty_message: String,
    pub map_state: String,
    /// The reader's capture state, verbatim ("FINAL", "FINAL (lower bounds)", "PROVISIONAL: ...").
    pub state: String,
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
    /// Report-ring (2047/11) fields as recorded by the reader: the CONFIG partition, the STATUS
    /// counters and the M2MF decoder rejection.  All empty when the capture is not ringed.
    pub ring_config: Option<RingConfigDrawn>,
    pub ring_status: Option<RingStatusDrawn>,
    pub ring_reject: Option<RingRejectDrawn>,
}

/// The report-ring partition negotiated in CONFIG (2047/11); `ps3dk-prof auto` "report ring: ...".
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RingConfigDrawn {
    pub blocks: u32,
    pub pairs_per_block: u32,
    pub first_report: u32,
    pub label: u32,
    pub main_bytes: u32,
    pub extents: u32,
    pub metadata_records: u32,
    pub legacy_pairs: u32,
}

/// The report-ring STATUS counters (2047/11); `ps3dk-prof auto` "report ring: blocks_issued ...".
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RingStatusDrawn {
    pub blocks_issued: u64,
    pub blocks_retired: u64,
    pub published_ring_label: u64,
    pub held_extents: u64,
    pub copied_bytes: u64,
    pub flushes_untouched: u64,
    pub flushes_restored: u64,
    pub decoded_words: u64,
}

/// The M2MF decoder's first rejected header (2047/11); `ps3dk-prof auto` "ring decoder rejected header ...".
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RingRejectDrawn {
    pub header: u32,
    pub offset: u32,
    /// `ps3dk_prof::auto_rsx::reject_reason(reason)` of the reader, verbatim.
    pub reason_text: String,
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
        ui.painter().rect(rect, egui::Rounding::same(9.0), fill, egui::Stroke::new(1.0_f32, outline));
        let text_pos = egui::pos2(rect.min.x + pad_x, rect.min.y + (rect.height() - text_size.y) * 0.5);
        ui.painter().galley(text_pos, galley, color);
    }
    response
}

fn auto_badge(ui: &mut egui::Ui, text: &str, tone: ui::Tone) -> egui::Response {
    verdict_hover(auto_badge_with_sense(ui, text, tone, Sense::hover()), text)
}

fn auto_badge_clickable(ui: &mut egui::Ui, text: &str, tone: ui::Tone) -> egui::Response {
    verdict_hover(auto_badge_with_sense(ui, text, tone, Sense::click()), text)
}

fn verdict_hover(response: egui::Response, text: &str) -> egui::Response {
    let key = text.split_once(':').map_or(text, |(key, _)| key);
    let verdict = [Verdict::RsxBound, Verdict::Pacing, Verdict::PpuBound, Verdict::Mixed, Verdict::Unknown]
        .into_iter().find(|v| key == v.key());
    match verdict {
        Some(v) => response.on_hover_text(v.explanation()),
        None => response,
    }
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

fn metric_text(v: &AutoView, mapped: Option<(u64, u64)>, raw: Option<u64>) -> String {
    match (mapped, raw) {
        (Some((lo, hi)), _) => bounds_ms((v.ticks_to_ms(lo), v.ticks_to_ms(hi))),
        (_, Some(t)) => format!("{t} raw"),
        _ => "-".into(),
    }
}
fn metric_tip(mapped: Option<(u64, u64)>, raw: Option<u64>) -> String {
    match mapped {
        Some((lo, hi)) => format!("Raw: {} RSX ticks | PPU: {lo}..{hi} ticks", raw.map_or("-".into(), |t| t.to_string())),
        None => "Unmapped clock: raw RSX ticks only".into(),
    }
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
    if v.report.frames.is_empty() {
        ui::field_message(p, ui::Tone::Neutral, "No AUTO frame rows retained: no coverage rows were recorded or the reader refused them; see the capture facts below.");
    }
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

        // Final vs Provisional / lower bounds.  The reader's state string names the verdict exactly.
        drawn.is_final = v.is_final();
        drawn.state = v.report.state.clone();
        fact_row(c, "Capture state", |v_ui| {
            v_ui.horizontal_top(|h| {
                // The badge names FINAL or PROVISIONAL; the text adds the reader's state only when it says more
                // than the badge (e.g. "FINAL (lower bounds)").
                let (badge, tone, plain) = if v.is_final() {
                    ("FINAL", ui::Tone::Success, "Closed with complete stop / finalization")
                } else {
                    ("PROVISIONAL", ui::Tone::Warning, "Provisional snapshot / open session")
                };
                auto_badge(h, badge, tone);
                h.add_space(ui::SPACE_XS);
                let text = if drawn.state == badge { plain.to_string() } else { drawn.state.clone() };
                h.add(egui::Label::new(&text).wrap()).on_hover_text(&text);
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

        // Report ring (2047/11): only ringed captures negotiate it.
        draw_report_ring(c, v, drawn);
    });
}

/// Adds the report-ring (2047/11) facts to the CAPTURE FACTS card: the CONFIG partition and the
/// STATUS counters (blocks issued/retired, held extents) plus the M2MF decoder's first rejection,
/// all read straight from the reader.  Absence of a row means the capture is not ringed.
fn draw_report_ring(c: &mut egui::Ui, v: &AutoView, drawn: &mut Drawn) {
    let Some(ring) = v.report.ring else {
        drawn.ring_config = None;
        drawn.ring_status = None;
        drawn.ring_reject = None;
        return;
    };
    // CONFIG partition: the reader's "report ring: ... (owner-buffer draws only)" line.  A ringed capture
    // always carries an ATTACHED config (the ring geometry is part of it).
    let metadata_records = v.metadata_records().unwrap_or(0);
    let config_text = format!(
        "{} blocks of {} pairs from report {}, label {}; budgets {} MAIN bytes in {} extents and {} metadata records; {} legacy pairs (owner-buffer draws only)",
        ring.blocks, ring.pairs, ring.first, ring.label, ring.bytes, ring.slots, metadata_records, ring.legacy_pairs
    );
    drawn.ring_config = Some(RingConfigDrawn {
        blocks: ring.blocks,
        pairs_per_block: ring.pairs,
        first_report: ring.first,
        label: ring.label,
        main_bytes: ring.bytes,
        extents: ring.slots,
        metadata_records,
        legacy_pairs: ring.legacy_pairs,
    });
    fact_row(c, "Report ring", |v_ui| {
        v_ui.horizontal_top(|h| {
            h.label(config_text.clone()).on_hover_text(&config_text);
        });
    });

    // STATUS counters: the reader's "report ring: blocks_issued ..., ..." line.
    let mut status_text = None;
    if let Some(t) = v.report.status.as_ref().and_then(|s| s.ring) {
        let fields: Vec<String> = ps3dk_prof::auto_rsx::RING_STATUS_FIELDS
            .iter().zip(t.iter()).map(|(n, val)| format!("{n} {val}"))
            .collect();
        status_text = Some(fields.join(", "));
        drawn.ring_status = Some(RingStatusDrawn {
            blocks_issued: t[0], blocks_retired: t[1], published_ring_label: t[2], held_extents: t[3],
            copied_bytes: t[4], flushes_untouched: t[5], flushes_restored: t[6], decoded_words: t[7],
        });
    }
    if let Some(text) = status_text {
        fact_row(c, "Report ring status", |v_ui| {
            v_ui.horizontal_top(|h| { h.label(&text).on_hover_text(&text); });
        });
    }

    // M2MF decoder's first rejected header: the reader's "ring decoder rejected header ..." line.
    if let Some([header, at, why]) = v.report.status.as_ref().and_then(|s| s.ring_reject).filter(|x| x[2] != 0) {
        let text = format!("ring decoder rejected header {header:#010x} at +{at:#x}: {}", reject_reason(why));
        drawn.ring_reject = Some(RingRejectDrawn { header, offset: at, reason_text: reject_reason(why).to_string() });
        fact_row(c, "Report ring decoder", |v_ui| {
            v_ui.horizontal_top(|h| {
                auto_badge(h, "REJECTED", ui::Tone::Danger);
                h.add_space(ui::SPACE_XS);
                h.label(&text).on_hover_text(&text);
            });
        });
    }
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
                tip.label(f.verdict.explanation());
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
        if visible_frames.is_empty() && !v.report.frames.is_empty() {
            ui::field_message(c, ui::Tone::Neutral, "No frames match this verdict filter; choose All to see the recorded frames.");
        }

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
                            let interval_text = format!("{interval_ms:.3} ms");
                            g.label(&interval_text).on_hover_text(format!("{interval_ticks} PPU ticks"));

                            let span_text = metric_text(v, f.rsx_span_ppu, f.rsx_span_raw);
                            g.label(&span_text).on_hover_text(metric_tip(f.rsx_span_ppu, f.rsx_span_raw));
                            let busy_text = metric_text(v, f.rsx_busy_ppu, f.rsx_busy_raw);
                            g.label(&busy_text).on_hover_text(metric_tip(f.rsx_busy_ppu, f.rsx_busy_raw));

                            // Flip-Wait / Poll shares
                            let presentation_text = if let Some(share) = presentation_share(f) {
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
                                g.label(&summary_str).on_hover_text(tooltip);
                                summary_str
                            } else {
                                g.label("-");
                                "-".into()
                            };

                            let ex_total: u64 = f.excluded.iter().sum();
                            let excluded_text = ex_total.to_string();
                            let exclusions = exclusions_breakdown(f);
                            let ring_exclusions = ring_exclusions_breakdown(f);
                            let mut all_exclusions = exclusions;
                            all_exclusions.extend(ring_exclusions.iter().cloned());
                            let tip_lines: Vec<String> = all_exclusions.iter()
                                .map(|(k, r, c)| format!("{k}: {c} ({r})")).collect();
                            let text = RichText::new(&excluded_text);
                            g.label(if ex_total > 0 { text.color(pal.warning) } else { text })
                                .on_hover_text(tip_lines.join("\n"));

                            let call_covered_text = show_call_covered.then(|| {
                                if f.call_covered > 0 { f.call_covered.to_string() } else { "-".into() }
                            });
                            if let Some(text) = &call_covered_text { g.label(text); }
                            g.add_sized([reason_col_w, 18.0], egui::Label::new(&f.reason).truncate())
                                .on_hover_text(&f.reason);
                            // Retain exactly the strings handed to the widgets, not a second
                            // independently formatted report that tests could accidentally bless.
                            drawn.rendered_frame_rows.push(RenderedRow {
                                frame: f.frame, verdict: f.verdict.key().into(), interval_text,
                                span_text, busy_text, reason: f.reason.clone(), excluded_text,
                                exclusions: all_exclusions, call_covered_text, presentation_text,
                            });

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
                    h.add(egui::Label::new(v.format_ms(AutoView::frame_interval_ticks(f))).wrap()).on_hover_text(&tb_text);
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
                let marks_text = f.rsx_frame_ppu.map_or_else(|| format!("{raw_marks} (raw RSX ticks; unmapped)"),
                    |(a, b)| bounds_ms((v.ticks_to_ms(a), v.ticks_to_ms(b))));
                let marks_tip = format!("{raw_marks} raw RSX ticks{mapped_marks}");
                g.horizontal_wrapped(|h| {
                    h.set_max_width(col2_max_w);
                    h.add(egui::Label::new(&marks_text).wrap()).on_hover_text(&marks_tip);
                });
                g.end_row();

                if let Some(fw) = f.fifo_wait {
                    g.label("FIFO Wait Evidence:");
                    // Round a lower bound down before displaying it at millisecond precision.
                    let us = u128::from(fw) * 1_000_000 / u128::from(v.tb_freq);
                    g.label(format!(">= {}.{:03} ms", us / 1000, us % 1000)).on_hover_text(format!(">= {fw} PPU ticks"));
                    g.end_row();
                }

                if f.call_brackets != 0 || f.call_covered != 0 {
                    g.label("CALL Brackets:");
                    g.label(format!("{} brackets · {} draws covered", f.call_brackets, f.call_covered));
                    g.end_row();
                }
            });

        // Exclusions breakdown table (the ten columns plus the report-ring columns of a 2047/11 frame)
        let mut breakdown = exclusions_breakdown(f);
        breakdown.extend(ring_exclusions_breakdown(f));
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

#[cfg(test)]
mod polish_tests {
    use super::*;

    fn text(out: &egui::FullOutput) -> String {
        fn walk(s: &egui::Shape, v: &mut String) {
            match s {
                egui::Shape::Text(t) => { v.push_str(&t.galley.job.text); v.push('\n'); },
                egui::Shape::Vec(xs) => xs.iter().for_each(|x| walk(x, v)),
                _ => {}
            }
        }
        let mut v = String::new();
        for s in &out.shapes { walk(&s.shape, &mut v); }
        v
    }

    #[test]
    fn verdict_hovers_use_reader_definitions() {
        for verdict in [Verdict::RsxBound, Verdict::PpuBound, Verdict::Pacing, Verdict::Mixed, Verdict::Unknown] {
            for counted in [false, true] {
                let ctx = egui::Context::default();
                ctx.style_mut(|s| { s.interaction.tooltip_delay = 0.0; s.interaction.tooltip_grace_time = 0.0; });
                let mut at = Pos2::new(20.0, 20.0);
                let mut painted = String::new();
                for i in 0..4 {
                    let out = ctx.run(egui::RawInput {
                        screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1000.0, 800.0))),
                        time: Some(i as f64),
                        events: vec![egui::Event::PointerMoved(at)],
                        ..Default::default()
                    }, |ctx| { egui::CentralPanel::default().show(ctx, |p| {
                        let label = if counted { format!("{}: 12", verdict.key()) } else { verdict.key().into() };
                        let response = if counted { auto_badge_clickable(p, &label, verdict_tone(verdict)) }
                            else { auto_badge(p, &label, verdict_tone(verdict)) };
                        at = response.rect.center();
                    }); });
                    painted = text(&out);
                }
                assert!(painted.contains(verdict.explanation()), "{} counted={counted}: {painted}", verdict.key());
            }
        }
    }
}
